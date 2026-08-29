#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <turbojpeg.h>

#include "app.h"
#include "net.h"
#include "posts.h"
#include "imgtex.h"
#include "bigview.h"
#include "thumbs.h"

#define THUMB_MAX_DIM 112
#define MAX_THUMBS    12

typedef enum { T_EMPTY, T_QUEUE, T_ACTIVE, T_READY, T_FAIL } TState;

typedef struct
{
    int post;
    TState st;
    u32 stamp;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
    dl_t *dl;
} Slot;

static Slot s_slots[MAX_THUMBS];
static u32 s_frame = 1;
static int s_ok = 0;
static int s_page = -1;

void thumbs_init(void) {}
void thumbs_exit(void) {}

void thumbs_reset(void)
{
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_ACTIVE && s->dl)
            dl_abort(s->dl);
        if (s->st == T_READY)
            C3D_TexDelete(&s->tex);
        memset(s, 0, sizeof(*s));
    }
    s_ok = 0;
    s_page = -1;
}

bool thumb_get(int post, C3D_Tex **tex, const Tex3DS_SubTexture **sub)
{
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_READY && s->post == post) {
            s->stamp = s_frame;
            *tex = &s->tex;
            *sub = &s->sub;
            return true;
        }
    }
    return false;
}

void thumbs_stats(int *ok, int *pending)
{
    int pend = 0;
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].st == T_QUEUE || s_slots[i].st == T_ACTIVE)
            pend++;
    if (ok)
        *ok = s_ok;
    if (pending)
        *pending = pend;
}

/* ------------------------------------------------------------------ */

static void build_url(int post, char *out, int outsz)
{
    Post *p = &posts[post];
    if (p->preview[0]) {
        if (!strncmp(p->preview, "//", 2))
            snprintf(out, outsz, "https:%s", p->preview);
        else if (p->preview[0] == '/')
            snprintf(out, outsz, "https://safebooru.org%s", p->preview);
        else
            snprintf(out, outsz, "%s", p->preview);
        return;
    }
    snprintf(out, outsz,
             "https://safebooru.org/thumbnails/%s/thumbnail_%s",
             p->directory, p->image);
}

static bool decode_thumb(u8 *jpg, u32 sz, C3D_Tex *tex, Tex3DS_SubTexture *sub)
{
    static tjhandle s_tj = NULL;
    if (!s_tj)
        s_tj = tjInitDecompress();
    if (!s_tj)
        return false;

    /* decoded thumb never exceeds 112x112 (static scratch, no heap churn) */
    static u8 s_rgba[128 * 128 * 4];

    bool ok = false;
    int W = 0, H = 0;
    if (tjDecompressHeader(s_tj, jpg, sz, &W, &H) == 0 && W > 0 && H > 0) {
        int num = 1, den = 1, nsf = 0;
        tjscalingfactor *sf = tjGetScalingFactors(&nsf);
        for (int i = 0; sf && i < nsf; i++) {
            if (sf[i].num > sf[i].denom)
                continue;
            int dw = (W * sf[i].num + sf[i].denom - 1) / sf[i].denom;
            int dh = (H * sf[i].num + sf[i].denom - 1) / sf[i].denom;
            if (dw <= THUMB_MAX_DIM && dh <= THUMB_MAX_DIM) {
                num = sf[i].num;
                den = sf[i].denom;
                break;
            }
        }
        int dw = (W * num + den - 1) / den;
        int dh = (H * num + den - 1) / den;

        if (dw <= 128 && dh <= 128 &&
            tjDecompress2(s_tj, jpg, sz, s_rgba, dw, 0, dh, TJPF_RGBA,
                          TJFLAG_FASTDCT) == 0)
            ok = imgtex_make565(tex, sub, s_rgba, dw, dh);
    }

    return ok;
}

static void free_slot(Slot *s)
{
    if (s->st == T_READY) {
        C3D_TexDelete(&s->tex);
        s_ok--;
    } else if (s->st == T_ACTIVE && s->dl) {
        dl_abort(s->dl);
    }
    memset(s, 0, sizeof(*s));
}

static Slot *alloc_slot(void)
{
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].st == T_EMPTY || s_slots[i].st == T_FAIL)
            return &s_slots[i];

    Slot *victim = NULL;
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_ACTIVE)
            continue;
        if (!victim || s->stamp < victim->stamp)
            victim = s;
    }
    if (victim)
        free_slot(victim);
    return victim;
}

static Slot *find_slot(int post)
{
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].post == post && s_slots[i].st != T_EMPTY)
            return &s_slots[i];
    return NULL;
}

static Slot *find_state(TState st)
{
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].st == st)
            return &s_slots[i];
    return NULL;
}

void thumbs_suspend(void)
{
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_ACTIVE && s->dl) {
            dl_abort(s->dl);
            s->dl = NULL;
            s->st = T_QUEUE;
        }
    }
}

void thumbs_update(int cursor)
{
    s_frame++;

    if (post_count <= 0)
        return;

    /* current page only */
    int cur_page = (cursor / PAGE_SIZE) * PAGE_SIZE;
    int page_end = cur_page + PAGE_SIZE;
    if (page_end > post_count)
        page_end = post_count;

    /* page changed: abort all downloads and reset page tracking.
       don't delete textures here — let alloc_slot evict them one at a
       time so linear memory stays stable on old3ds. */
    if (cur_page != s_page) {
        for (int i = 0; i < MAX_THUMBS; i++) {
            Slot *s = &s_slots[i];
            if (s->st == T_ACTIVE && s->dl) {
                dl_abort(s->dl);
                s->dl = NULL;
                s->st = T_EMPTY;
            }
        }
        s_ok = 0;
        s_page = cur_page;
    }

    /* retry failed thumbnails after a delay (5 seconds) */
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_FAIL && s_frame - s->stamp > 300)
            s->st = T_QUEUE;
    }

    /* queue missing ones for the current page */
    for (int i = cur_page; i < page_end; i++) {
        if (find_slot(i))
            continue;
        Slot *s = alloc_slot();
        if (!s)
            break;
        memset(s, 0, sizeof(*s));
        s->post = i;
        s->st = T_QUEUE;
        s->stamp = s_frame;
    }

    /* start next download (single download, prioritise cursor) */
    if (!find_state(T_ACTIVE) && !bigview_busy()) {
        Slot *nextq = NULL;
        int best_dist = INT_MAX;
        for (int i = 0; i < MAX_THUMBS; i++) {
            Slot *s = &s_slots[i];
            if (s->st != T_QUEUE)
                continue;
            int dist = s->post - cursor;
            if (dist < 0) dist = -dist;
            if (dist < best_dist) {
                best_dist = dist;
                nextq = s;
            }
        }
        if (nextq) {
            char url[512];
            build_url(nextq->post, url, sizeof(url));
            nextq->dl = dl_start(url);
            nextq->st = nextq->dl ? T_ACTIVE : T_FAIL;
            if (!nextq->dl)
                nextq->stamp = s_frame;
        }
    }

    /* pump active download */
    Slot *act = find_state(T_ACTIVE);
    if (act && act->dl) {
        int r = dl_pump(act->dl);
        if (r == DL_DONE) {
            u8 *buf = (u8 *)dl_buf(act->dl);
            u32 sz = dl_size(act->dl);
            bool ok = sz >= 16 && decode_thumb(buf, sz, &act->tex, &act->sub);
            dl_abort(act->dl);
            act->dl = NULL;
            act->st = ok ? T_READY : T_FAIL;
            if (ok)
                s_ok++;
            else
                act->stamp = s_frame;
        } else if (r == DL_ERR) {
            dl_abort(act->dl);
            act->dl = NULL;
            act->st = T_FAIL;
            act->stamp = s_frame;
        }
    }
}

bool thumbs_page_ready(int cursor)
{
    int first = (cursor / PAGE_SIZE) * PAGE_SIZE;
    int end = first + PAGE_SIZE;
    if (end > post_count)
        end = post_count;
    for (int i = first; i < end; i++) {
        Slot *s = find_slot(i);
        if (!s || s->st != T_READY)
            return false;
    }
    return true;
}
