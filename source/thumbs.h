#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>

/* async thumbnail cache: one network transfer at a time, decoded on a
   second core; call thumbs_update(cursor) every frame */
void thumbs_init(void);
void thumbs_exit(void);
void thumbs_reset(void);
void thumbs_suspend(void); /* abort in-flight transfer, keep ready thumbs */
void thumbs_update(int cursor);
bool thumb_get(int post, C3D_Tex **tex, const Tex3DS_SubTexture **sub);
void thumbs_stats(int *ok, int *pending);
