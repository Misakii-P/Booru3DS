#pragma once
#include <citro2d.h>

/* false if the text buffer could not be allocated - nothing can be drawn
   in that case and the caller should bail out */
bool ui_init(C3D_RenderTarget **top, C3D_RenderTarget **bot);
void ui_exit(void);
void render_frame(void);
