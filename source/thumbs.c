#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <turbojpeg.h>

#include "app.h"
#include "net.h"
#include "posts.h"
#include "imgtex.h"
#include "thumbs.h"

#define THUMB_MAX_DIM 112
#define MAX_THUMBS    30

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

static Slot *find_state(TState st)
{
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].st == st)
            return &s_slots[i];
    return NULL;
}

static Slot *find_slot(int post)
{
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].post == post && s_slots[i].st != T_EMPTY)
            return &s_slots[i];
    return NULL;
}

void thumbs_suspend(void)
{
    Slot *s = find_state(T_ACTIVE);
    if (s && s->dl) {
        dl_abort(s->dl);
        s->dl = NULL;
        s->st = T_QUEUE; /* retry later */
    }
}

void thumbs_update(int cursor)
{
    s_frame++;

    if (post_count <= 0)
        return;

    /* desired set: cursor first, then outward; current page + next page */
    int first_page = (cursor / PAGE_SIZE) * PAGE_SIZE;
    int last_page = first_page + 2 * PAGE_SIZE;
    if (last_page > post_count)
        last_page = post_count;

    int want[2 * PAGE_SIZE];
    int nwant = 0;
    want[nwant++] = cursor;
    for (int k = 1; k < 2 * PAGE_SIZE && nwant < 2 * PAGE_SIZE; k++) {
        if (cursor - k >= first_page)
            want[nwant++] = cursor - k;
        if (nwant < 2 * PAGE_SIZE && cursor + k < last_page)
            want[nwant++] = cursor + k;
    }

    /* touch wanted ready slots so they survive eviction */
    for (int i = 0; i < nwant; i++) {
        Slot *s = find_slot(want[i]);
        if (s)
            s->stamp = s_frame;
    }

    /* queue missing ones */
    for (int i = 0; i < nwant; i++) {
        if (find_slot(want[i]))
            continue;
        Slot *s = alloc_slot();
        if (!s)
            break;
        memset(s, 0, sizeof(*s));
        s->post = want[i];
        s->st = T_QUEUE;
        s->stamp = s_frame;
    }

    /* start next download if none in flight */
    if (!find_state(T_ACTIVE)) {
        Slot *nextq = find_state(T_QUEUE);
        if (nextq) {
            char url[512];
            build_url(nextq->post, url, sizeof(url));
            nextq->dl = dl_start(url);
            nextq->st = nextq->dl ? T_ACTIVE : T_FAIL;
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
            dl_abort(act->dl); /* frees buffer + struct */
            act->dl = NULL;
            act->st = ok ? T_READY : T_FAIL;
            if (ok)
                s_ok++;
        } else if (r == DL_ERR) {
            dl_abort(act->dl);
            act->dl = NULL;
            act->st = T_FAIL;
        }
    }
}
