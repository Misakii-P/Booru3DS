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

static struct
{
    int post;      /* currently loaded */
    int want;      /* requested */
    bool ready;
    bool failed;
    dl_t *dl;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
} s_big = { -1, -1, false, false, NULL, {}, {} };

void bigview_reset(void)
{
    if (s_big.dl)
        dl_abort(s_big.dl);
    if (s_big.ready || s_big.failed)
        C3D_TexDelete(&s_big.tex);
    memset(&s_big, 0, sizeof(s_big));
    s_big.post = -1;
    s_big.want = -1;
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
    if (s_big.dl) {
        dl_abort(s_big.dl);
        s_big.dl = NULL;
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
        snprintf(out, outsz, "https://safebooru.org%s", cand);
    else
        snprintf(out, outsz, "%s", cand);
}

/* largest supported downscale fitting the top screen budget */
static bool decode_big(u8 *jpg, u32 sz, C3D_Tex *tex, Tex3DS_SubTexture *sub)
{
    static tjhandle s_tj = NULL;
    if (!s_tj)
        s_tj = tjInitDecompress();
    if (!s_tj)
        return false;

    /* decoded big view never exceeds 400x240 (static scratch) */
    static u8 s_rgba[BIG_MAX_W * BIG_MAX_H * 4];

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
            if (dw <= BIG_MAX_W && dh <= BIG_MAX_H) {
                num = sf[i].num;
                den = sf[i].denom;
                break;
            }
        }
        int dw = (W * num + den - 1) / den;
        int dh = (H * num + den - 1) / den;

        if (tjDecompress2(s_tj, jpg, sz, s_rgba, dw, 0, dh, TJPF_RGBA,
                          TJFLAG_FASTDCT) == 0)
            ok = imgtex_make(tex, sub, s_rgba, dw, dh);
    }

    return ok;
}

void bigview_pump(void)
{
    if (post_count <= 0 || s_big.want < 0 || s_big.want >= post_count)
        return;

    /* start fetch when idle and the wanted post differs from loaded */
    if (!s_big.dl && !s_big.ready && !s_big.failed &&
        s_big.want != s_big.post) {
        char url[512];
        build_url(s_big.want, url, sizeof(url));
        if (!url[0]) {
            s_big.failed = true;
            return;
        }
        s_big.dl = dl_start(url);
        if (!s_big.dl)
            s_big.failed = true;
        return;
    }

    if (s_big.dl) {
        int r = dl_pump(s_big.dl);
        if (r == DL_DONE) {
            u8 *buf = (u8 *)dl_buf(s_big.dl);
            u32 sz = dl_size(s_big.dl);
            bool ok = sz >= 16 &&
                      decode_big(buf, sz, &s_big.tex, &s_big.sub);
            dl_abort(s_big.dl);
            s_big.dl = NULL;
            s_big.post = ok ? s_big.want : -1;
            s_big.ready = ok;
            s_big.failed = !ok;
        } else if (r == DL_ERR) {
            dl_abort(s_big.dl);
            s_big.dl = NULL;
            s_big.failed = true;
        }
    }
}
