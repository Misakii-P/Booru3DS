#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>

#include "imgtex.h"

static u32 next_pow2(u32 v)
{
    u32 r = 8;
    while (r < v)
        r <<= 1;
    return r;
}

/* PICA 8x8 tile order (verified byte-exact against tex3ds output):
   linear source index l = fy*8+fx  ->  morton8(fy,fx) destination index */
static const u8 SRC2DST[64] = {
    0, 1, 4, 5, 16, 17, 20, 21,
    2, 3, 6, 7, 18, 19, 22, 23,
    8, 9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63,
};

/* RGBA -> tiled texture. Padding is edge-replicated so GPU_LINEAR sampling
   at the subrect border blends with real pixels instead of black zeros */
static bool imgtex_common(C3D_Tex *tex, Tex3DS_SubTexture *sub,
                          const u8 *rgba, int w, int h, bool fmt565)
{
    u32 wp = next_pow2(w), hp = next_pow2(h);

    /* static scratch: worst case 512x256 (400x240 big view + padding),
       avoids heap churn and fragmentation on old3ds */
    static u8 s_pad[512 * 256 * 4];
    if (wp > 512 || hp > 256)
        return false;

    /* padded source with edge replication */
    u8 *pad = s_pad;
    for (u32 y = 0; y < hp; y++) {
        const u8 *row = rgba + (size_t)(y < (u32)h ? y : h - 1) * w * 4;
        for (u32 x = 0; x < wp; x++)
            memcpy(pad + (y * wp + x) * 4,
                   row + (size_t)(x < (u32)w ? x : w - 1) * 4, 4);
    }

    if (!C3D_TexInit(tex, wp, hp, fmt565 ? GPU_RGB565 : GPU_RGBA8))
        return false;
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
    tex->border = 0;
    memset(tex->data, 0, wp * hp * (fmt565 ? 2 : 4));

    if (fmt565) {
        u16 *dst = (u16 *)tex->data;
        for (u32 y = 0; y < hp; y++) {
            int ty = y >> 3;
            int lin = (y & 7) << 3;
            const u8 *src = pad + y * wp * 4;
            for (u32 x = 0; x < wp; x++) {
                const u8 *px = src + x * 4;
                u32 off = (((ty * (int)(wp >> 3)) + (int)(x >> 3)) << 6)
                        + SRC2DST[lin + (x & 7)];
                dst[off] = (u16)((px[0] >> 3) << 11 | (px[1] >> 2) << 5
                               | (px[2] >> 3));
            }
        }
    } else {
        /* texels are stored [A,B,G,R] in memory */
        u32 *dst = (u32 *)tex->data;
        for (u32 y = 0; y < hp; y++) {
            int ty = y >> 3;
            int lin = (y & 7) << 3;
            const u8 *src = pad + y * wp * 4;
            for (u32 x = 0; x < wp; x++) {
                const u8 *px = src + x * 4;
                u32 off = (((ty * (int)(wp >> 3)) + (int)(x >> 3)) << 6)
                        + SRC2DST[lin + (x & 7)];
                dst[off] = (u32)px[0] << 24 | (u32)px[1] << 16
                         | (u32)px[2] << 8 | (u32)px[3];
            }
        }
    }
    C3D_TexFlush(tex);

    /* tex3ds convention: top > bottom (top < bottom means "rotated"),
       v axis inverted vs storage: image top row is at v = 1 */
    sub->width = w;
    sub->height = h;
    sub->left = 0.0f;
    sub->top = 1.0f;
    sub->right = w / (float)wp;
    sub->bottom = 1.0f - h / (float)hp;
    return true;
}

bool imgtex_make(C3D_Tex *tex, Tex3DS_SubTexture *sub,
                 const u8 *rgba, int w, int h)
{
    return imgtex_common(tex, sub, rgba, w, h, false);
}

bool imgtex_make565(C3D_Tex *tex, Tex3DS_SubTexture *sub,
                    const u8 *rgba, int w, int h)
{
    return imgtex_common(tex, sub, rgba, w, h, true);
}
