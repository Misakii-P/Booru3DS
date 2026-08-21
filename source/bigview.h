#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>

/* big top-screen image: fetches sample/file url for the cursor post */
void bigview_reset(void);
void bigview_abort(void);
void bigview_request(int post);
bool bigview_get(int post, C3D_Tex **tex, const Tex3DS_SubTexture **sub);
void bigview_pump(void);
