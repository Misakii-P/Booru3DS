#include <stdio.h>
#include <stdarg.h>

#include <3ds.h>
#include <citro2d.h>
#include <tex3ds.h>

#include "app.h"
#include "posts.h"
#include "thumbs.h"
#include "bigview.h"
#include "save.h"
#include "ui.h"
#include "topscreenbg_t3x.h"
#include "konachanbg_t3x.h"

/* colors: light/white theme */
#define COL_BG      C2D_Color32(255, 255, 255, 255)
#define COL_BAR     C2D_Color32(233, 236, 241, 255)
#define COL_CARD    C2D_Color32(246, 248, 251, 255)
#define COL_LINE    C2D_Color32(212, 216, 224, 255)
#define COL_BTN     C2D_Color32(228, 232, 238, 255)
#define COL_ACCENT  C2D_Color32(64, 150, 220, 255)
#define COL_SEL     C2D_Color32(64, 150, 220, 60)
#define COL_TEXT    C2D_Color32(36, 38, 44, 255)
#define COL_DIM     C2D_Color32(140, 144, 152, 255)

static C3D_RenderTarget *s_top, *s_bot;
static C2D_TextBuf s_tbuf;
static C2D_SpriteSheet s_bgsheet;      /* safebooru title bg */
static C2D_SpriteSheet s_konachanbg;   /* konachan title bg */

void ui_init(C3D_RenderTarget **top, C3D_RenderTarget **bot)
{
    s_top = *top;
    s_bot = *bot;
    s_tbuf = C2D_TextBufNew(8192);
    s_bgsheet = C2D_SpriteSheetLoadFromMem(
        topscreenbg_t3x,
        (size_t)(topscreenbg_t3x_end - topscreenbg_t3x));
    s_konachanbg = C2D_SpriteSheetLoadFromMem(
        konachanbg_t3x,
        (size_t)(konachanbg_t3x_end - konachanbg_t3x));
}

void ui_exit(void)
{
    if (s_tbuf) C2D_TextBufDelete(s_tbuf);
    if (s_bgsheet) C2D_SpriteSheetFree(s_bgsheet);
    if (s_konachanbg) C2D_SpriteSheetFree(s_konachanbg);
    s_tbuf = NULL;
    s_bgsheet = NULL;
    s_konachanbg = NULL;
}

static void draw_text_d(float x, float y, float scale, u32 color, u32 flags,
                        float depth, const char *fmt, ...)
{
    static char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    C2D_TextBufClear(s_tbuf);
    C2D_Text t;
    C2D_TextFontParse(&t, NULL, s_tbuf, tmp);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, depth, scale, scale, color);
}

static void draw_text(float x, float y, float scale, u32 color, u32 flags,
                      const char *fmt, ...)
{
    static char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    C2D_TextBufClear(s_tbuf);
    C2D_Text t;
    C2D_TextFontParse(&t, NULL, s_tbuf, tmp);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
}

static void draw_image_fit(C2D_Image img, float cx, float cy,
                           float maxw, float maxh, float max_scale)
{
    float iw = img.subtex->width, ih = img.subtex->height;
    float sc = iw / maxw > ih / maxh ? maxw / iw : maxh / ih;
    if (sc > max_scale)
        sc = max_scale;
    C2D_DrawImageAt(img, cx - iw * sc / 2, cy - ih * sc / 2,
                    0.5f, NULL, sc, sc);
}

static void draw_home_icon(float x, float y, float s, u32 col)
{
    /* roof */
    C2D_DrawTriangle(x, y + s * 0.5f, col,
                     x + s / 2, y, col,
                     x + s, y + s * 0.5f, col, 0.5f);
    /* body */
    C2D_DrawRectSolid(x + s * 0.18f, y + s * 0.45f, 0.5f,
                      s * 0.64f, s * 0.55f, col);
    /* door */
    C2D_DrawRectSolid(x + s * 0.42f, y + s * 0.68f, 0.6f,
                      s * 0.16f, s * 0.32f, COL_BG);
}

static void render_top(void)
{
    if (screen == SCR_HOME) {
        C2D_SpriteSheet sheet = g_provider == 0 ? s_bgsheet : s_konachanbg;
        if (!sheet)
            sheet = s_bgsheet;
        if (sheet) {
            C2D_Image bg = C2D_SpriteSheetGetImage(sheet, 0);
            C2D_DrawImageAt(bg, 0, 0, 0.5f, NULL, 1.0f, 1.0f);
        }
        return;
    }

    C3D_Tex *tex = NULL;
    const Tex3DS_SubTexture *sub = NULL;
    if (bigview_get(cursor, &tex, &sub)) {
        C2D_Image img = { tex, sub };
        draw_image_fit(img, 200, 114, 396, 222, 1.0f);
    } else if (thumb_get(cursor, &tex, &sub)) {
        /* big version still loading: show the thumb upscaled meanwhile */
        C2D_Image img = { tex, sub };
        draw_image_fit(img, 200, 114, 396, 222, 4.0f);
        draw_text(200, 226, 0.34f, COL_DIM, C2D_AlignCenter,
                  "loading full size...");
    } else {
        draw_text(200, 108, 0.55f, COL_DIM, C2D_AlignCenter,
                  "%s", g_status[0] ? g_status : "loading...");
    }

    int ok = 0, pend = 0;
    thumbs_stats(&ok, &pend);
    draw_text(4, 228, 0.34f, COL_DIM, C2D_AlignLeft,
              "%s%s%d/%d", g_status, pend > 0 ? " " : "", ok, post_count);
}

static void draw_cell(int idx, float x, float y, bool sel)
{
    /* card */
    C2D_DrawRectSolid(x, y, 0.30f, CELL_W - 2, CELL_H - 2, sel ? COL_ACCENT : COL_LINE);
    C2D_DrawRectSolid(x + 1, y + 1, 0.31f, CELL_W - 4, CELL_H - 4, COL_CARD);
    if (sel) {
        C2D_DrawRectSolid(x + 2, y + 2, 0.32f, CELL_W - 6, CELL_H - 6, COL_SEL);
    }

    C3D_Tex *tex = NULL;
    const Tex3DS_SubTexture *sub = NULL;
    if (thumb_get(idx, &tex, &sub)) {
        C2D_Image img = { tex, sub };
        draw_image_fit(img, x + CELL_W / 2, y + CELL_H / 2,
                       CELL_W - 12, CELL_H - 12, 1.0f);
    } else {
        draw_text(x + CELL_W / 2, y + CELL_H / 2 - 7, 0.42f, COL_LINE,
                  C2D_AlignCenter, "...");
    }
}

static void draw_save_prompt(void);

static void render_bottom(void)
{
    if (screen == SCR_HOME) {
        draw_text(BOT_W / 2, 60, 0.75f, COL_TEXT, C2D_AlignCenter,
                  "Booru3DS");
        draw_text(BOT_W / 2, 88, 0.42f, COL_DIM, C2D_AlignCenter,
                  "%s", g_provider_name);

        /* search bar */
        C2D_DrawRectSolid(40, 104, 0.4f, 240, 40, COL_BAR);
        C2D_DrawRectSolid(40, 104, 0.45f, 240, 3, COL_ACCENT);
        draw_text(BOT_W / 2, 118, 0.5f, COL_DIM, C2D_AlignCenter,
                  "Tap to search");

        draw_text(BOT_W / 2, 214, 0.42f, COL_DIM, C2D_AlignCenter,
                  "SELECT: site   X: search   START: exit");

        /* status/errors are otherwise invisible on the title screen */
        if (g_status[0])
            draw_text(BOT_W / 2, 186, 0.42f,
                      strncmp(g_status, "search failed", 13) == 0
                          ? C2D_Color32(210, 70, 60, 255)
                          : COL_DIM,
                      C2D_AlignCenter, "%s", g_status);
        return;
    }

    /* top bar: home button + search bar */
    C2D_DrawRectSolid(HOME_BTN_X, HOME_BTN_Y, 0.4f,
                      HOME_BTN_S, HOME_BTN_S, COL_BTN);
    draw_home_icon(HOME_BTN_X + 6, HOME_BTN_Y + 6, HOME_BTN_S - 12, COL_TEXT);

    C2D_DrawRectSolid(SBAR_LX, SBAR_TY, 0.4f,
                      SBAR_RX - SBAR_LX, SBAR_BY - SBAR_TY, COL_BAR);
    C2D_DrawRectSolid(SBAR_LX, SBAR_BY - 3, 0.45f,
                      SBAR_RX - SBAR_LX, 3, COL_ACCENT);
    draw_text(SBAR_LX + 8, SBAR_TY + 10, 0.45f,
              current_tags[0] ? COL_TEXT : COL_DIM, C2D_AlignLeft,
              "%s", current_tags[0] ? current_tags : "search...");

    /* save status (right side of search bar) */
    SaveState sv = save_state();
    if (sv == SAVE_ACTIVE)
        draw_text(SBAR_RX - 8, SBAR_TY + 10, 0.42f, COL_DIM, C2D_AlignRight,
                  "saving %lu KB", (unsigned long)(save_bytes() / 1024));
    else if (sv == SAVE_OK)
        draw_text(SBAR_RX - 8, SBAR_TY + 10, 0.42f, COL_ACCENT,
                  C2D_AlignRight, "saved!");
    else if (sv == SAVE_ERR)
        draw_text(SBAR_RX - 8, SBAR_TY + 10, 0.42f, C2D_Color32(210, 70, 60, 255),
                  C2D_AlignRight, "save failed");

    /* thumbnail grid */
    int first_page = (cursor / PAGE_SIZE) * PAGE_SIZE;
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int i = first_page + r * GRID_COLS + c;
            if (i >= post_count)
                break;
            draw_cell(i, GRID_X + c * CELL_W + 1,
                      GRID_Y + r * CELL_H + 1, i == cursor);
        }
    }

    if (g_save_prompt)
        draw_save_prompt();
}

static void draw_save_prompt(void)
{
    /* topmost band: above grid images (0.5) and normal text (0.5);
       dialog text itself sits at 0.9 via draw_text_d */
    C2D_DrawRectSolid(0, 0, 0.60f, BOT_W, 240, C2D_Color32(0, 0, 0, 140));

    float px = (BOT_W - 280) / 2, py = 70, pw = 280, ph = 100;
    C2D_DrawRectSolid(px - 2, py - 2, 0.62f, pw + 4, ph + 4, COL_ACCENT);
    C2D_DrawRectSolid(px, py, 0.63f, pw, ph, COL_CARD);

    draw_text_d(BOT_W / 2, py + 10, 0.5f, COL_TEXT, C2D_AlignCenter,
                0.9f, "Save image");
    draw_text_d(BOT_W / 2, py + 32, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "Install the image to the 3DS camera app");
    draw_text_d(BOT_W / 2, py + 48, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "or the /3ds/booru folder?");

    draw_text_d(px + 14, py + 72, 0.45f, COL_ACCENT, C2D_AlignLeft,
                0.9f, "A: booru");
    draw_text_d(BOT_W / 2, py + 72, 0.45f, COL_ACCENT, C2D_AlignCenter,
                0.9f, "X: camera");
    draw_text_d(px + pw - 14, py + 72, 0.45f, COL_DIM, C2D_AlignRight,
                0.9f, "B: cancel");
}

void render_frame(void)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s_top, COL_BG);
    C2D_SceneBegin(s_top);
    render_top();
    C2D_TargetClear(s_bot, COL_BG);
    C2D_SceneBegin(s_bot);
    render_bottom();
    C3D_FrameEnd(0);
}
