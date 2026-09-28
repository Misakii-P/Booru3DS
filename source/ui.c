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
#include "sdata.h"
#include "bgm.h"
#include "net.h"
#include "ui.h"
#include "topscreenbg_t3x.h"
#include "topsearch_t3x.h"
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
#define COL_INK     C2D_Color32(22, 22, 26, 255)

static C3D_RenderTarget *s_top, *s_bot;
static C2D_TextBuf s_tbuf;
static C2D_SpriteSheet s_bgsheet;      /* safebooru title bg */
static C2D_SpriteSheet s_konachanbg;   /* konachan title bg */
static C2D_SpriteSheet s_searchbg;     /* results screen bg */

bool ui_init(C3D_RenderTarget **top, C3D_RenderTarget **bot)
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
    s_searchbg = C2D_SpriteSheetLoadFromMem(
        topsearch_t3x,
        (size_t)(topsearch_t3x_end - topsearch_t3x));
    /* the backgrounds are cosmetic and every draw path already handles a
       NULL sheet, but without a text buffer nothing can be drawn at all */
    return s_tbuf != NULL;
}

void ui_exit(void)
{
    if (s_tbuf) C2D_TextBufDelete(s_tbuf);
    if (s_bgsheet) C2D_SpriteSheetFree(s_bgsheet);
    if (s_konachanbg) C2D_SpriteSheetFree(s_konachanbg);
    if (s_searchbg) C2D_SpriteSheetFree(s_searchbg);
    s_tbuf = NULL;
    s_bgsheet = NULL;
    s_konachanbg = NULL;
    s_searchbg = NULL;
}

static void draw_text_d(float x, float y, float scale, u32 color, u32 flags,
                        float depth, const char *fmt, ...)
{
    if (!s_tbuf)
        return;
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
    if (!s_tbuf)
        return;
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

/* ring strip from a0 to a1 (radians, screen coords: +angle = clockwise) */
static void draw_arc(float cx, float cy, float r, float thick,
                     float a0, float a1, u32 col, float depth)
{
    const int SEG = 16;
    float ri = r - thick;
    for (int i = 0; i < SEG; i++) {
        float b0 = a0 + (a1 - a0) * i / SEG;
        float b1 = a0 + (a1 - a0) * (i + 1) / SEG;
        float ox0 = cx + cosf(b0) * r,     oy0 = cy + sinf(b0) * r;
        float ox1 = cx + cosf(b1) * r,     oy1 = cy + sinf(b1) * r;
        float ix0 = cx + cosf(b0) * ri,    iy0 = cy + sinf(b0) * ri;
        float ix1 = cx + cosf(b1) * ri,    iy1 = cy + sinf(b1) * ri;
        C2D_DrawTriangle(ox0, oy0, col, ix0, iy0, col, ox1, oy1, col, depth);
        C2D_DrawTriangle(ix0, iy0, col, ix1, iy1, col, ox1, oy1, col, depth);
    }
}

/* square button with a blue border */
static void draw_framed_button(float x, float y, float s)
{
    C2D_DrawRectSolid(x, y, 0.40f, s, s, COL_ACCENT);
    C2D_DrawRectSolid(x + 2, y + 2, 0.41f, s - 4, s - 4, COL_BTN);
}

/* thin blue-bordered search bar */
static void draw_search_bar(float x, float y, float w, float h)
{
    C2D_DrawRectSolid(x, y, 0.40f, w, h, COL_ACCENT);
    C2D_DrawRectSolid(x + 2, y + 2, 0.41f, w - 4, h - 4, COL_BAR);
}

/* black clock wrapped by a circular arrow hugging it */
static void draw_history_icon(float cx, float cy, float r)
{
    /* hands: minute up (longer), hour right */
    C2D_DrawRectSolid(cx - 1.5f, cy - r * 0.88f, 0.55f,
                      3, r * 0.98f, COL_INK);
    C2D_DrawRectSolid(cx, cy - 1.5f, 0.55f,
                      r * 0.72f, 3, COL_INK);

    /* circular arrow: arc around the clock, gap at top-right */
    float R = r + 4.0f, thick = 3.0f;
    float a0 = -0.1745f, a1 = 5.585f;           /* 340 deg sweep */
    draw_arc(cx, cy, R, thick, a0, a1, COL_INK, 0.55f);

    /* head anchored at the arc end, aligned with its clockwise tangent */
    float fx = -sinf(a1), fy = cosf(a1);
    float nx = cosf(a1),  ny = sinf(a1);
    float px = cx + nx * R, py = cy + ny * R;
    float hl = 6.5f, hw = 3.6f;

    C2D_DrawTriangle(px + fx * hl, py + fy * hl, COL_INK,
                     px + nx * hw, py + ny * hw, COL_INK,
                     px - nx * hw, py - ny * hw, COL_INK, 0.56f);
}

static void draw_history_button(void)
{
    draw_framed_button((float)HIST_BTN_X, (float)HIST_BTN_Y,
                       (float)HIST_BTN_S);
    draw_history_icon(HIST_BTN_X + HIST_BTN_S / 2.0f,
                      HIST_BTN_Y + HIST_BTN_S / 2.0f,
                      HIST_BTN_S / 2.0f - 9);
}

/* document sheet with a folded corner and text lines
   (proportions follow the Material "description" icon: ~4:5 sheet,
   fold taking ~35% of the width at the top-right) */
static void draw_document_icon(float cx, float cy)
{
    const float w = 22, h = 28, f = 7.5f;
    float L = cx - w / 2, T = cy - h / 2;

    /* body with the top-right corner clipped */
    C2D_DrawRectSolid(L, T, 0.55f, w - f, h, COL_INK);
    C2D_DrawRectSolid(L + w - f, T + f, 0.55f, f, h - f, COL_INK);

    /* folded corner flap, shaded lighter for depth */
    C2D_DrawTriangle(L + w - f, T,     COL_DIM,
                     L + w,     T + f, COL_DIM,
                     L + w - f, T + f, COL_DIM,
                     0.56f);

    /* text lines carved in button colour */
    for (int i = 0; i < 3; i++) {
        float ly = T + 11 + i * 5.5f;
        float lw = (i == 2) ? w - 12 : w - 8;
        C2D_DrawRectSolid(L + 4, ly, 0.56f, lw, 3, COL_BTN);
    }
}

static void draw_about_button(void)
{
    draw_framed_button((float)ABOUT_BTN_X, (float)ABOUT_BTN_Y,
                       (float)ABOUT_BTN_S);
    draw_document_icon(ABOUT_BTN_X + ABOUT_BTN_S / 2.0f,
                       ABOUT_BTN_Y + ABOUT_BTN_S / 2.0f);
}

/* speaker with waves (on) or slash (muted) */
static void draw_sound_icon(float cx, float cy, bool on)
{
    /* driver box */
    C2D_DrawRectSolid(cx - 9, cy - 5, 0.55f, 6, 10, COL_INK);

    /* cone (trapezoid from two triangles) */
    C2D_DrawTriangle(cx - 4, cy - 7, COL_INK,
                     cx + 3, cy - 12, COL_INK,
                     cx + 3, cy + 12, COL_INK, 0.55f);
    C2D_DrawTriangle(cx - 4, cy - 7, COL_INK,
                     cx + 3, cy + 12, COL_INK,
                     cx - 4, cy + 7, COL_INK, 0.55f);

    if (on) {
        /* sound waves on the right side */
        draw_arc(cx - 2, cy, 8.0f, 2.0f, -0.85f, 0.85f, COL_INK, 0.56f);
        draw_arc(cx - 2, cy, 11.5f, 2.0f, -0.85f, 0.85f, COL_INK, 0.57f);
    } else {
        /* red diagonal slash across the speaker */
        u32 red = C2D_Color32(205, 65, 55, 255);
        float ax = cx + 10, ay = cy - 10;
        float bx = cx - 4, by = cy + 4;
        float nx = -1.414f, ny = -1.414f; /* perpendicular * 2 */
        C2D_DrawTriangle(ax + nx, ay + ny, red,
                         bx + nx, by + ny, red,
                         bx - nx, by - ny, red, 0.58f);
        C2D_DrawTriangle(ax + nx, ay + ny, red,
                         bx - nx, by - ny, red,
                         ax - nx, ay - ny, red, 0.58f);
    }
}

static void draw_sound_button(void)
{
    draw_framed_button((float)SND_BTN_X, (float)SND_BTN_Y,
                       (float)SND_BTN_S);
    draw_sound_icon(SND_BTN_X + SND_BTN_S / 2.0f,
                    SND_BTN_Y + SND_BTN_S / 2.0f,
                    bgm_on());
}

static void draw_about_overlay(void)
{
    C2D_DrawRectSolid(0, 0, 0.60f, BOT_W, 240, C2D_Color32(0, 0, 0, 140));

    float px = 12, py = 34, pw = 296, ph = 172;
    C2D_DrawRectSolid(px - 2, py - 2, 0.62f, pw + 4, ph + 4, COL_ACCENT);
    C2D_DrawRectSolid(px, py, 0.63f, pw, ph, COL_CARD);

    draw_text_d(BOT_W / 2, py + 12, 0.46f, COL_TEXT, C2D_AlignCenter,
                0.9f, "Booru3DS - a simple Booru client");
    draw_text_d(BOT_W / 2, py + 30, 0.46f, COL_TEXT, C2D_AlignCenter,
                0.9f, "for the Nintendo 3DS");

    draw_text_d(BOT_W / 2, py + 62, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "This app and its music has been");
    draw_text_d(BOT_W / 2, py + 78, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "created by MisakiP_.");

    draw_text_d(BOT_W / 2, py + 102, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "I am not affiliated with Safebooru or");
    draw_text_d(BOT_W / 2, py + 118, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "Konachan. Use at your own risk.");

    draw_text_d(BOT_W / 2, py + 146, 0.38f, COL_ACCENT, C2D_AlignCenter,
                0.9f, "github.com/Misakii-P/Booru3DS");
}

static void draw_camwarn_overlay(void)
{
    C2D_DrawRectSolid(0, 0, 0.60f, BOT_W, 240, C2D_Color32(0, 0, 0, 150));

    float px = 12, py = 28, pw = 296, ph = 184;
    C2D_DrawRectSolid(px - 2, py - 2, 0.62f, pw + 4, ph + 4, COL_ACCENT);
    C2D_DrawRectSolid(px, py, 0.63f, pw, ph, COL_CARD);

    draw_text_d(BOT_W / 2, py + 10, 0.48f, COL_TEXT, C2D_AlignCenter,
                0.9f, "Image installed to camera");

    draw_text_d(BOT_W / 2, py + 40, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "If you don't see your images");
    draw_text_d(BOT_W / 2, py + 56, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "in the gallery, follow the");
    draw_text_d(BOT_W / 2, py + 72, 0.42f, COL_DIM, C2D_AlignCenter,
                0.9f, "troubleshooting steps at:");

    draw_text_d(BOT_W / 2, py + 98, 0.40f, COL_ACCENT, C2D_AlignCenter,
                0.9f, "github.com/Adrix12team/SCR2JPG");
    draw_text_d(BOT_W / 2, py + 114, 0.40f, COL_ACCENT, C2D_AlignCenter,
                0.9f, "#troubleshooting");

    draw_text_d(BOT_W / 2, py + ph - 18, 0.38f, COL_DIM, C2D_AlignCenter,
                0.9f, "press any button to close");
}

static void draw_hist_overlay(void)
{
    int n = sdata_hist_count();

    C2D_DrawRectSolid(0, 0, 0.60f, BOT_W, 240, C2D_Color32(0, 0, 0, 140));

    float px = 20, py = 30, pw = 280, ph = 190;
    C2D_DrawRectSolid(px - 2, py - 2, 0.62f, pw + 4, ph + 4, COL_ACCENT);
    C2D_DrawRectSolid(px, py, 0.63f, pw, ph, COL_CARD);

    draw_text_d(BOT_W / 2, py + 8, 0.5f, COL_TEXT, C2D_AlignCenter,
                0.9f, "Search history");

    if (n == 0) {
        draw_text_d(BOT_W / 2, py + 60, 0.46f, COL_DIM, C2D_AlignCenter,
                    0.9f, "No recent searches.");
    } else {
        for (int i = 0; i < n; i++) {
            float ry = py + 34 + i * 18;
            if (i == g_hist_sel)
                C2D_DrawRectSolid(px + 6, ry - 2, 0.64f, pw - 12, 17,
                                  COL_SEL);
            draw_text_d(px + 12, ry, 0.42f,
                        i == g_hist_sel ? COL_TEXT : COL_DIM,
                        C2D_AlignLeft, 0.9f, "%.34s",
                        sdata_hist_get(i) ? sdata_hist_get(i) : "");
        }
    }

    draw_text_d(BOT_W / 2, py + ph - 16, 0.38f, COL_DIM, C2D_AlignCenter,
                0.9f, "A / tap: search   B: close");
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

    /* results background */
    if (s_searchbg) {
        C2D_Image bg = C2D_SpriteSheetGetImage(s_searchbg, 0);
        C2D_DrawImageAt(bg, 0, 0, 0.5f, NULL, 1.0f, 1.0f);
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
        if (bigview_failed())
            draw_text(200, 4, 0.34f, COL_DIM, C2D_AlignCenter,
                      "full size unavailable: %.26s", g_big_err);
        else
            draw_text(200, 4, 0.34f, COL_DIM, C2D_AlignCenter,
                      "loading full size... %lu KB",
                      (unsigned long)(bigview_bytes() / 1024));
    } else {
        draw_text(200, 108, 0.55f, COL_DIM, C2D_AlignCenter,
                  "%s", g_status[0] ? g_status : "loading...");
    }

    draw_text(4, 4, 0.32f, COL_DIM, C2D_AlignLeft,
              "%d/%d", cursor + 1, post_count);
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
                  "%s", provider_name());

        /* search bar */
        draw_search_bar(40, 104, 228, 40);
        draw_text(154, 118, 0.5f, COL_DIM, C2D_AlignCenter,
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
    } else {

    /* top bar: home button + search bar */
    draw_framed_button((float)HOME_BTN_X, (float)HOME_BTN_Y,
                       (float)HOME_BTN_S);
    draw_home_icon(HOME_BTN_X + 6, HOME_BTN_Y + 6, HOME_BTN_S - 12, COL_TEXT);

    draw_search_bar((float)SBAR_LX, (float)SBAR_TY,
                    (float)(SBAR_RX - SBAR_LX), (float)(SBAR_BY - SBAR_TY));
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
    }

    /* common: modal layers on every screen; corner buttons on title only */
    if (screen == SCR_HOME && !g_searching) {
        draw_history_button();
        draw_about_button();
    }
    if (!g_searching)
        draw_sound_button();

    if (g_save_prompt)
        draw_save_prompt();
    else if (g_hist_open)
        draw_hist_overlay();
    else if (g_about_open)
        draw_about_overlay();
    else if (g_camwarn_open)
        draw_camwarn_overlay();
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
