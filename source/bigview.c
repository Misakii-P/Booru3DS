#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <turbojpeg.h>

#include "net.h"
#include "posts.h"
#include "imgtex.h"
#include "bigview.h"

#define BIG_MAX_W 400
#define BIG_MAX_H 240
#define BIG_RETRY_FRAMES 300 /* ~5s at 60fps */

static struct
{
    int post;      /* currently loaded, and which post it belongs to */
    int want;      /* what the cursor is asking for */
    int fetching;  /* post the in-flight transfer is actually for; want
                      can move underneath it when a request is coalesced */
    bool ready;
    bool failed;
    u32 retry_at;  /* frame stamp; failures are retried, not sticky */
    dl_t *dl;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
} s_big = { -1, -1, -1, false, false, 0, NULL, {}, {} };

static u32 s_frame = 1;
static tjhandle s_tj;
static char s_err[192] = "";
const char *g_big_err = s_err;

void bigview_init(void)
{
    s_tj = tjInitDecompress();
}

void bigview_exit(void)
{
    bigview_reset();
    if (s_tj) {
        tjDestroy(s_tj);
        s_tj = NULL;
    }
}

void bigview_reset(void)
{
    if (s_big.dl) {
        dl_abort(s_big.dl);
        s_big.dl = NULL;
    }
    if (s_big.ready)
        C3D_TexDelete(&s_big.tex);
    memset(&s_big, 0, sizeof(s_big));
    s_big.post = -1;
    s_big.want = -1;
    s_big.fetching = -1;
    s_err[0] = 0;
}

void bigview_abort(void)
{
    if (s_big.dl) {
        dl_abort(s_big.dl);
        s_big.dl = NULL;
        s_big.failed = false; /* retry when re-requested */
    }
}

void bigview_request(int post)
{
    if (s_big.want == post)
        return;
    /* if a download is in flight, coalesce: just update want, let it
       finish and the next pump will start the new one. Avoids abort churn
       on mass touches. */
    if (s_big.dl) {
        s_big.want = post;
        s_big.failed = false;
        return;
    }
    if (s_big.ready) {
        C3D_TexDelete(&s_big.tex);
        s_big.ready = false;
    }
    s_big.failed = false;
    s_big.want = post;
}

bool bigview_get(int post, C3D_Tex **tex, const Tex3DS_SubTexture **sub)
{
    if (!s_big.ready || s_big.post != post)
        return false;
    *tex = &s_big.tex;
    *sub = &s_big.sub;
    return true;
}

static void build_url(int post, char *out, int outsz)
{
    Post *p = &posts[post];
    const char *cand = p->sample[0] ? p->sample : p->file;
    if (!cand || !cand[0]) {
        out[0] = 0;
        return;
    }
    if (!strncmp(cand, "//", 2))
        snprintf(out, outsz, "https:%s", cand);
    else if (cand[0] == '/')
        snprintf(out, outsz, "https://%s%s", net_host(), cand);
    else
        snprintf(out, outsz, "%s", cand);
}

/* largest supported downscale fitting the top screen budget */
static bool decode_big(u8 *jpg, u32 sz, C3D_Tex *tex, Tex3DS_SubTexture *sub)
{
    if (!s_tj)
        return false;

    int W = 0, H = 0;
    if (tjDecompressHeader(s_tj, jpg, sz, &W, &H) != 0 || W <= 0 || H <= 0)
        return false;

    int num = 1, den = 1, nsf = 0;
    tjscalingfactor *sf = tjGetScalingFactors(&nsf);
    for (int i = 0; sf && i < nsf; i++) {
        if (sf[i].num > sf[i].denom)
            continue;
        int dw = (W * sf[i].num + sf[i].denom - 1) / sf[i].denom;
        int dh = (H * sf[i].num + sf[i].denom - 1) / sf[i].denom;
        if (dw <= BIG_MAX_W && dh <= BIG_MAX_H) {
            num = sf[i].num;
            den = sf[i].denom;
            break;
        }
    }
    int dw = (W * num + den - 1) / den;
    int dh = (H * num + den - 1) / den;

    /* When no scale factor fits - anything taller than 1920 or wider than
       3200, which is most full-size originals - num/den are still 1/1 and
       dw/dh are the full image. Decoding that into a screen-sized buffer
       overruns it by orders of magnitude, so refuse instead. */
    if (dw > BIG_MAX_W || dh > BIG_MAX_H)
        return false;

    /* sized to the actual decode rather than the worst case, and freed
       straight after: a fixed 384KB scratch in BSS is worth having back
       on the heap the rest of the time */
    u8 *rgba = (u8 *)malloc((size_t)dw * dh * 4);
    if (!rgba)
        return false;

    bool ok = tjDecompress2(s_tj, jpg, sz, rgba, dw, 0, dh, TJPF_RGBA,
                            TJFLAG_FASTDCT) == 0 &&
             imgtex_make(tex, sub, rgba, dw, dh);
    free(rgba);
    return ok;
}

bool bigview_busy(void)
{
    return s_big.dl != NULL;
}

bool bigview_failed(void)
{
    return s_big.failed;
}

u32 bigview_bytes(void)
{
    return s_big.dl ? dl_size(s_big.dl) : 0;
}

void bigview_pump(void)
{
    s_frame++;

    if (post_count <= 0 || s_big.want < 0 || s_big.want >= post_count)
        return;

    /* a failure is not permanent: give it a few seconds and try again,
       the same way thumbnails do */
    if (s_big.failed && s_big.want != s_big.post &&
        s_frame - s_big.retry_at >= BIG_RETRY_FRAMES) {
        s_big.failed = false;
        s_err[0] = 0;
    }

    /* Start a fetch whenever what we hold is not what the cursor wants.
       That includes the case where a finished transfer is now stale
       because a request was coalesced onto it - otherwise the texture
       for the old post would sit there and block its own replacement. */
    if (!s_big.dl && !s_big.failed && s_big.want != s_big.post) {
        if (s_big.ready) {
            C3D_TexDelete(&s_big.tex);
            s_big.ready = false;
        }
        char url[512];
        build_url(s_big.want, url, sizeof(url));
        if (!url[0]) {
            s_big.failed = true;
            s_big.retry_at = s_frame;
            return;
        }
        s_big.dl = dl_start(url, false);
        if (!s_big.dl) {
            s_big.failed = true;
            s_big.retry_at = s_frame;
        } else {
            s_big.fetching = s_big.want;
        }
        return;
    }

    if (s_big.dl) {
        int r = dl_pump(s_big.dl);
        if (r == DL_DONE) {
            long code = dl_code(s_big.dl);
            u8 *buf = (u8 *)dl_buf(s_big.dl);
            u32 sz = dl_size(s_big.dl);
            bool ok = sz >= 16 &&
                      decode_big(buf, sz, &s_big.tex, &s_big.sub);
            if (ok) {
                s_err[0] = 0;
            } else {
                snprintf(s_err, sizeof(s_err), "%s (http %ld%s)",
                         dl_err(s_big.dl), code,
                         sz < 16 ? ", empty" : "too large");
            }
            dl_abort(s_big.dl);
            s_big.dl = NULL;
            /* credit the texture to the post this transfer was started
               for, not to want: a request that arrived mid-flight moved
               want on, and attributing these bytes to it would paint the
               previous post's image under the new cursor */
            s_big.post = ok ? s_big.fetching : -1;
            s_big.ready = ok;
            s_big.failed = !ok;
            s_big.retry_at = s_frame;
        } else if (r == DL_ERR) {
            snprintf(s_err, sizeof(s_err), "%s", dl_err(s_big.dl));
            dl_abort(s_big.dl);
            s_big.dl = NULL;
            s_big.failed = true;
            s_big.retry_at = s_frame;
        }
    }
}
