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
#include "scaledec.h"
#include "thumbs.h"

#define THUMB_MAX_DIM  112
#define THUMB_MAX_EDGE 128 /* must match the decode scratch below */
#define MAX_THUMBS     12
/* Wall-clock here is network latency, not bandwidth: a cold TLS connect to
   safebooru costs a few hundred ms, so a page of twelve thumbs is
   dominated by how many handshakes are in flight at once rather than by
   throughput. Three keeps the SOC buffer and the per-frame decode budget
   comfortable while cutting page-fill time to roughly a third. */
#define THUMB_CONCURRENT    3
#define THUMB_RETRY_FRAMES  300 /* ~5s at 60fps */
#define THUMB_MAX_TRIES     3    /* give up so page changes can proceed */

typedef enum { T_EMPTY, T_QUEUE, T_ACTIVE, T_READY, T_FAIL } TState;

typedef struct
{
    int post;
    TState st;
    u32 stamp;
    u8 tries; /* consecutive failures; past THUMB_MAX_TRIES this slot
                 stops holding up a page change */
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
    dl_t *dl;
} Slot;

static Slot s_slots[MAX_THUMBS];
static u32 s_frame = 1;
static int s_page = -1;
static tjhandle s_tj;

void thumbs_init(void)
{
    s_tj = tjInitDecompress();
}

void thumbs_exit(void)
{
    thumbs_reset();
    if (s_tj) {
        tjDestroy(s_tj);
        s_tj = NULL;
    }
}

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

/* ------------------------------------------------------------------ */

static void build_url(int post, char *out, int outsz)
{
    Post *p = &posts[post];
    if (p->preview[0]) {
        if (!strncmp(p->preview, "//", 2))
            snprintf(out, outsz, "https:%s", p->preview);
        else if (p->preview[0] == '/')
            snprintf(out, outsz, "https://%s%s", net_host(), p->preview);
        else
            snprintf(out, outsz, "%s", p->preview);
        return;
    }
    snprintf(out, outsz,
             "https://%s/thumbnails/%s/thumbnail_%s",
             net_host(), p->directory, p->image);
}

static bool decode_thumb(u8 *jpg, u32 sz, C3D_Tex *tex, Tex3DS_SubTexture *sub)
{
    if (!s_tj)
        return false;

    /* decoded thumb never exceeds 112x112 (static scratch, no heap churn) */
    static u8 s_rgba[THUMB_MAX_EDGE * THUMB_MAX_EDGE * 4];

    bool ok = false;
    int W = 0, H = 0;
    if (tjDecompressHeader(s_tj, jpg, sz, &W, &H) == 0 && W > 0 && H > 0) {
        int dw = 0, dh = 0;
        /* refuses rather than decoding at 1/1, which would overrun the
           scratch above - see scale_fit() */
        if (scale_fit(W, H, THUMB_MAX_DIM, THUMB_MAX_DIM, &dw, &dh) &&
            dw <= THUMB_MAX_EDGE && dh <= THUMB_MAX_EDGE &&
            tjDecompress2(s_tj, jpg, sz, s_rgba, dw, 0, dh, TJPF_RGBA,
                          TJFLAG_FASTDCT) == 0)
            ok = imgtex_make565(tex, sub, s_rgba, dw, dh);
    }

    return ok;
}

static void free_slot(Slot *s)
{
    if (s->st == T_READY)
        C3D_TexDelete(&s->tex);
    else if (s->st == T_ACTIVE && s->dl)
        dl_abort(s->dl);
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

static int count_active(void)
{
    int n = 0;
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].st == T_ACTIVE)
            n++;
    return n;
}

/* the queued slot closest to the cursor, so the selection fills first */
static Slot *pick_queued(int cursor)
{
    Slot *best = NULL;
    int best_dist = INT_MAX;
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st != T_QUEUE)
            continue;
        int dist = s->post - cursor;
        if (dist < 0) dist = -dist;
        if (dist < best_dist) {
            best_dist = dist;
            best = s;
        }
    }
    return best;
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
        s_page = cur_page;
    }

    /* retry failures after a delay, but only a few times: a post that is
       gone for good must eventually stop blocking page changes */
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_FAIL && s->tries < THUMB_MAX_TRIES &&
            s_frame - s->stamp > THUMB_RETRY_FRAMES)
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

    /* Start transfers, cursor first. These deliberately keep running while
       the big view is fetching: the two are limited by latency, not by
       bandwidth, so serialising them just added the big view's round trip
       to every page fill. */
    while (count_active() < THUMB_CONCURRENT) {
        Slot *nextq = pick_queued(cursor);
        if (!nextq)
            break;
        char url[512];
        build_url(nextq->post, url, sizeof(url));
        nextq->dl = dl_start(url, false);
        nextq->st = nextq->dl ? T_ACTIVE : T_FAIL;
        if (!nextq->dl) {
            nextq->stamp = s_frame;
            nextq->tries++;
        }
    }

    /* Pump every active transfer, but decode at most one image per frame.
       Decoding runs here on the main thread, and while it does nothing
       else happens - including the curl_multi_perform inside dl_pump that
       is what actually drives the network. A burst of three decodes would
       hitch the frame and stall the transfers that had not landed yet.
       Anything skipped stays DL_DONE and is picked up next frame. */
    bool decoded = false;
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st != T_ACTIVE || !s->dl)
            continue;
        int r = dl_pump(s->dl);
        if (r == DL_ERR) {
            dl_abort(s->dl);
            s->dl = NULL;
            s->st = T_FAIL;
            s->stamp = s_frame;
            s->tries++;
            continue;
        }
        if (r != DL_DONE || decoded)
            continue;
        decoded = true;
        u8 *buf = (u8 *)dl_buf(s->dl);
        u32 sz = dl_size(s->dl);
        bool ok = sz >= 16 && decode_thumb(buf, sz, &s->tex, &s->sub);
        dl_abort(s->dl);
        s->dl = NULL;
        s->st = ok ? T_READY : T_FAIL;
        if (!ok) {
            s->stamp = s_frame;
            s->tries++;
        }
    }
}
