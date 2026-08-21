#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>

/* decode-ready RGBA -> tiled GPU texture (verified vs tex3ds output).
   pow2 padding is edge-replicated so linear filtering never bleeds black */
bool imgtex_make(C3D_Tex *tex, Tex3DS_SubTexture *sub,
                 const u8 *rgba, int w, int h);

/* RGB565 variant (half memory/bandwidth, no alpha) */
bool imgtex_make565(C3D_Tex *tex, Tex3DS_SubTexture *sub,
                    const u8 *rgba, int w, int h);
