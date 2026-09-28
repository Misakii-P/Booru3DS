#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <sys/stat.h>

#include <3ds.h>
#include <citro2d.h>

#include "app.h"
#include "net.h"
#include "posts.h"
#include "thumbs.h"
#include "bigview.h"
#include "save.h"
#include "sdata.h"
#include "ui.h"
#include "bgm.h"
#include "sfx.h"

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

int cursor = 0;
char current_tags[128] = "";
Screen screen = SCR_HOME;

char g_status[256] = "";

/* API providers: safebooru (gelbooru dapi) works from emulators with a PC
   network stack, but its CDN 403-blocks real 3DS TLS. konachan.net serves
   the same style of content and accepts the 3DS's requests. The table
   (names + hosts) lives in net.c so every URL builder resolves relative
   paths against the site that is actually selected. */
int g_provider = 0;
bool g_save_prompt = false;
bool g_about_open = false;
bool g_camwarn_open = false;
bool g_searching = false;
bool g_hist_open = false;
int g_hist_sel = 0;

static C3D_RenderTarget *s_top, *s_bot;

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

static dl_t *s_search_dl = NULL;

static int do_search(void)
{
    if (g_searching) return -1;
    char enc[512];
    char url[1024];

    g_searching = true;

    /* free sockets so the search request has a clean connection */
    thumbs_suspend();
    bigview_abort();

    snprintf(g_status, sizeof(g_status), "searching...");
    render_frame();

    url_encode(current_tags, enc, sizeof(enc));
    if (g_provider == 0)
        snprintf(url, sizeof(url),
                 "https://safebooru.org/index.php?page=dapi&s=post&q=index"
                 "&json=1&limit=%d&tags=%s", MAX_POSTS, enc);
    else
        snprintf(url, sizeof(url),
                 "https://konachan.net/post.json?limit=%d&tags=%s",
                 MAX_POSTS, enc);

    /* the search response is JSON, so it is the one transfer worth
       asking for compressed */
    s_search_dl = dl_start(url, true);
    if (!s_search_dl) {
        g_searching = false;
        post_count = 0;
        cursor = 0;
        snprintf(g_status, sizeof(g_status), "search failed");
        render_frame();
        return -1;
    }
    return 0;
}

static void go_home(void)
{
    current_tags[0] = 0;
    do_search(); /* front page = the "home images" */
}

static void prompt_search(void)
{
    static SwkbdState swkbd;
    char tmp[128];

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, sizeof(tmp) - 1);
    swkbdSetHintText(&swkbd, "Tag(s) to search");
    SwkbdButton btn = swkbdInputText(&swkbd, tmp, sizeof(tmp));
    if (btn == SWKBD_BUTTON_CONFIRM) {
        strncpy(current_tags, tmp, sizeof(current_tags) - 1);
        current_tags[sizeof(current_tags) - 1] = 0;
        do_search();
    }
    render_frame();
}

static bool in_rect(int px, int py, float x, float y, float w, float h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* touch debounce: ignore taps faster than 100ms (prevents mass-touch OOM) */
static u32 s_lastTouch = 0;

static void handle_touch(int px, int py)
{
    u32 now = svcGetSystemTick() / 268123; /* ~ms, CPU 268MHz */
    if (now - s_lastTouch < 100) return;
    s_lastTouch = now;

    /* sound toggle, top-right on every screen */
    if (!g_searching &&
        in_rect(px, py, SND_BTN_X, SND_BTN_Y, SND_BTN_S, SND_BTN_S)) {
        sfx_click();
        bgm_set_on(!bgm_on());
        return;
    }

    /* title screen: corner buttons + search bar */
    if (screen == SCR_HOME && !g_searching) {
        if (in_rect(px, py, HIST_BTN_X, HIST_BTN_Y, HIST_BTN_S, HIST_BTN_S)) {
            sfx_click();
            g_hist_sel = 0;
            g_hist_open = true;
            return;
        }
        if (in_rect(px, py, ABOUT_BTN_X, ABOUT_BTN_Y, ABOUT_BTN_S,
                    ABOUT_BTN_S)) {
            sfx_click();
            g_about_open = true;
            return;
        }
        if (in_rect(px, py, 40, 104, 228, 40)) {
            sfx_click();
            prompt_search();
            return;
        }
        return;
    }

    if (screen == SCR_LIST && g_searching)
        return;

    /* results screen: top bar (home + search) then the grid */
    if (in_rect(px, py, HOME_BTN_X, HOME_BTN_Y, HOME_BTN_S, HOME_BTN_S)) {
        sfx_click();
        go_home();
        return;
    }
    if (in_rect(px, py, SBAR_LX, SBAR_TY, SBAR_RX - SBAR_LX,
                SBAR_BY - SBAR_TY)) {
        sfx_click();
        prompt_search();
        return;
    }

    if (py >= GRID_Y) {
        int col = (px - GRID_X) / CELL_W;
        int row = (py - GRID_Y) / CELL_H;
        if (col < GRID_COLS && row < GRID_ROWS) {
            int first_page = (cursor / PAGE_SIZE) * PAGE_SIZE;
            int i = first_page + row * GRID_COLS + col;
            if (i < post_count && i != cursor) {
                sfx_click(); /* art touched */
                cursor = i;
                bigview_request(cursor);
            }
        }
    }
}

/* ------------------------------------------------------------------ */

static void move_cursor(int delta)
{
    if (post_count <= 0)
        return;

    int c = cursor + delta;
    if (c < 0) c = 0;
    if (c > post_count - 1) c = post_count - 1;
    if (c != cursor) {
        /* Move immediately. Waiting for all twelve thumbnails of the page
           we are leaving made paging feel broken, and it bought nothing:
           the LRU cache already keeps them, so coming back is instant,
           and thumbs that are still in flight simply keep filling in. */
        cursor = c;
        bigview_request(cursor);
    }
}

/* The Circle Pad and C-Stick report every frame, so they need their own
   repeat throttle; the D-Pad is already edge-triggered via hidKeysDown()
   and throttling it would swallow legitimate presses. */
static u32 s_lastMove = 0;

static void move_cursor_analog(int delta)
{
    u32 now = svcGetSystemTick() / 268123;
    if (now - s_lastMove < 80) return;
    s_lastMove = now;
    move_cursor(delta);
}

/* libcurl resolves DNS via newlib's resolver, which needs /etc/resolv.conf
   on the SD root (httpc did this inside the system module instead) */
static void net_dns_init(void)
{
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f) {
        fclose(f);
        return;
    }
    mkdir("/etc", 0777);
    f = fopen("/etc/resolv.conf", "w");
    if (!f)
        return;
    fputs("nameserver 1.1.1.1\nnameserver 8.8.8.8\n", f);
    fclose(f);
}

/* BSD socket service for libcurl - 256KB covers the per-connection socket
   structs for every transfer that can be in flight at once (2 thumbnails
   plus the big view or a save), and saves 256KB of heap on OLD3DS
   (64MB total) */
#define SOC_ALIGN 0x1000
#define SOC_SIZE  0x40000
static u32 *s_soc = NULL;
static bool s_soc_ready = false;

static bool net_soc_init(void)
{
    s_soc = (u32 *)memalign(SOC_ALIGN, SOC_SIZE);
    if (!s_soc)
        return false;
    memset(s_soc, 0, SOC_SIZE);
    if (R_FAILED(socInit(s_soc, SOC_SIZE))) {
        free(s_soc);
        s_soc = NULL;
        return false;
    }
    s_soc_ready = true;
    return true;
}

/* every init step is individually optional, so teardown has to be safe
   to run from a partially-built state - that is what the flags are for */
/* The HOME menu suspends the app rather than killing it, so aptMainLoop()
   keeps returning true and every static in the process survives - but the
   ndsp queue does not. Without this hook the audio modules never learn
   about it and playback comes back distorted after re-entry. */
static void on_apt_hook(APT_HookType hook, void *param)
{
    (void)param;
    switch (hook) {
    case APTHOOK_ONSUSPEND:
        bgm_suspend();
        sfx_suspend();
        break;
    case APTHOOK_ONRESTORE:
        bgm_resume();
        sfx_resume();
        break;
    default:
        break;
    }
}

static aptHookCookie s_apt_cookie;

static bool s_romfs_ok, s_c3d_ok, s_c2d_ok;

static void teardown(void)
{
    /* stop the hook first: it must not fire into audio modules that are
       about to be torn down */
    aptUnhook(&s_apt_cookie);

    if (s_search_dl) {
        dl_abort(s_search_dl);
        s_search_dl = NULL;
    }
    thumbs_exit();
    bigview_exit();
    save_exit();
    sfx_exit();
    bgm_exit();
    sdata_exit();
    ui_exit();
    if (s_c2d_ok) {
        C2D_Fini();
        s_c2d_ok = false;
    }
    if (s_c3d_ok) {
        C3D_Fini();
        s_c3d_ok = false;
    }
    if (s_romfs_ok) {
        romfsExit();
        s_romfs_ok = false;
    }
    net_exit(); /* after every dl_t has been aborted */
    if (s_soc_ready) {
        socExit();
        s_soc_ready = false;
    }
    free(s_soc);
    s_soc = NULL;
    gfxExit();
}

int main(void)
{
    gfxInitDefault();

    if (R_FAILED(romfsInit())) {
        gfxExit();
        return 1;
    }
    s_romfs_ok = true;

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE))
        goto fail;
    s_c3d_ok = true;

    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS))
        goto fail;
    s_c2d_ok = true;
    C2D_Prepare();

    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!s_top || !s_bot)
        goto fail;
    if (!ui_init(&s_top, &s_bot))
        goto fail;

    thumbs_init();
    bigview_init();
    save_init();
    net_dns_init();
    if (!net_soc_init())
        snprintf(g_status, sizeof(g_status), "soc init failed");

    bgm_init();
    sfx_init();
    if (!bgm_play("romfs:/bgm.ogg"))
        snprintf(g_status, sizeof(g_status), "bgm not loaded");

    /* after the audio modules exist, so the first suspend cannot race
       their initialisation */
    aptHook(&s_apt_cookie, on_apt_hook, NULL);

    sdata_load();
    g_provider = sdata_provider();
    if (g_provider < 0 || g_provider >= PV_COUNT)
        g_provider = 0;

    render_frame();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (g_camwarn_open) {
            /* first camera-install notice: anything dismisses it */
            if (kDown & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START |
                         KEY_SELECT | KEY_TOUCH))
                g_camwarn_open = false;
        } else if (g_save_prompt) {
            /* save-destination dialog */
            if (kDown & KEY_A) {
                g_save_prompt = false;
                save_request(cursor, SAVE_DEST_BOORU);
            } else if (kDown & KEY_X) {
                g_save_prompt = false;
                save_request(cursor, SAVE_DEST_CAMERA);
            } else if (kDown & KEY_B) {
                g_save_prompt = false;
            }
        } else if (g_about_open) {
            /* about popup: any button or tap dismisses it */
            if (kDown & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START |
                         KEY_SELECT | KEY_TOUCH))
                g_about_open = false;
        } else if (g_hist_open) {
            /* search-history overlay */
            int n = sdata_hist_count();
            if (n > 0) {
                if ((kDown & KEY_DUP) && g_hist_sel > 0)
                    g_hist_sel--;
                if ((kDown & KEY_DDOWN) && g_hist_sel < n - 1)
                    g_hist_sel++;
                if (kDown & KEY_A) {
                    const char *h = sdata_hist_get(g_hist_sel);
                    if (h) {
                        strncpy(current_tags, h,
                                sizeof(current_tags) - 1);
                        current_tags[sizeof(current_tags) - 1] = 0;
                        g_hist_open = false;
                        do_search();
                    }
                }
            }
            if (kDown & (KEY_B | KEY_START))
                g_hist_open = false;

            if (kDown & KEY_TOUCH) {
                touchPosition tp;
                hidTouchRead(&tp);
                /* tap a row to search it; anywhere else closes.
                   geometry must mirror draw_hist_overlay(): rows start at
                   panel.y + 34, 18px pitch */
                float ry0 = 30.0f + 34.0f;
                int row = (int)((tp.py - ry0) / 18.0f);
                if (tp.px >= 20 && tp.px <= 300 && row >= 0 && row < n &&
                    tp.py >= ry0 && tp.py < ry0 + n * 18.0f) {
                    g_hist_sel = row;
                    const char *h = sdata_hist_get(row);
                    if (h) {
                        strncpy(current_tags, h,
                                sizeof(current_tags) - 1);
                        current_tags[sizeof(current_tags) - 1] = 0;
                        g_hist_open = false;
                        do_search();
                    }
                } else if (!in_rect(tp.px, tp.py, 20, 30, 280, 190)) {
                    g_hist_open = false;
                }
            }
        } else {
            if (kDown & KEY_START)
                break;

            if ((kDown & KEY_A) && screen == SCR_LIST)
                g_save_prompt = true;

            if (kDown & KEY_X)
                prompt_search();

            /* like the other modals, history stays shut during a search:
               picking an entry while one is in flight is a silent no-op */
            if ((kDown & KEY_Y) && !g_searching && sdata_hist_count() > 0) {
                g_hist_sel = 0;
                g_hist_open = true;
            }

            if ((kDown & KEY_B) && screen == SCR_LIST)
                go_home();

            if (kDown & KEY_SELECT) {
                g_provider = (g_provider + 1) % PV_COUNT;
                sdata_set_provider(g_provider);
                current_tags[0] = 0;
                screen = SCR_HOME;
                thumbs_reset();
                bigview_reset();
                save_reset();
                snprintf(g_status, sizeof(g_status), "site: %s",
                         provider_name());
            }

            if (kDown & KEY_TOUCH) {
                touchPosition tp;
                hidTouchRead(&tp);
                handle_touch(tp.px, tp.py);
            }

            if (kDown & KEY_DUP)
                move_cursor(-GRID_COLS);
            if (kDown & KEY_DDOWN)
                move_cursor(GRID_COLS);
            if (kDown & KEY_DLEFT)
                move_cursor(-1);
            if (kDown & KEY_DRIGHT)
                move_cursor(1);
            if (kDown & KEY_L)
                move_cursor(-PAGE_SIZE);
            if (kDown & KEY_R)
                move_cursor(PAGE_SIZE);

            /* Circle Pad (and New3DS C-Stick) */
            circlePosition cpos;
            hidCircleRead(&cpos);
            if (abs(cpos.dx) > 55 || abs(cpos.dy) > 55) {
                if (abs(cpos.dx) > abs(cpos.dy)) {
                    if (cpos.dx > 55) move_cursor_analog(1);
                    else if (cpos.dx < -55) move_cursor_analog(-1);
                } else {
                    if (cpos.dy > 55) move_cursor_analog(-GRID_COLS);
                    else if (cpos.dy < -55) move_cursor_analog(GRID_COLS);
                }
            }
            circlePosition cspos;
            hidCstickRead(&cspos);
            if (abs(cspos.dx) > 55 || abs(cspos.dy) > 55) {
                if (abs(cspos.dx) > abs(cspos.dy)) {
                    if (cspos.dx > 55) move_cursor_analog(1);
                    else if (cspos.dx < -55) move_cursor_analog(-1);
                } else {
                    if (cspos.dy > 55) move_cursor_analog(-GRID_COLS);
                    else if (cspos.dy < -55) move_cursor_analog(GRID_COLS);
                }
            }
        }

        /* pump async search download */
        if (s_search_dl) {
            int r = dl_pump(s_search_dl);
            if (r == DL_DONE) {
                u8 *buf = (u8 *)dl_buf(s_search_dl);
                u32 sz = dl_size(s_search_dl);
                long code = dl_code(s_search_dl);
                char err[128];
                snprintf(err, sizeof(err), "%s", dl_err(s_search_dl));
                dl_abort(s_search_dl);
                s_search_dl = NULL;

                if (code != 200 || sz == 0) {
                    post_count = 0;
                    cursor = 0;
                    snprintf(g_status, sizeof(g_status),
                             "fail(%ld) http:%ld %s",
                             code, code, err);
                    g_searching = false;
                    render_frame();
                } else {
                    parse_posts((char *)buf);

                    cursor = 0;
                    screen = SCR_LIST;
                    thumbs_reset();
                    bigview_reset();
                    save_reset();
                    bigview_request(cursor);
                    sdata_hist_push(current_tags);
                    g_searching = false;

                    if (post_count == 0)
                        snprintf(g_status, sizeof(g_status),
                                 "no results");
                    else
                        g_status[0] = 0;
                }
            } else if (r == DL_ERR) {
                long code = dl_code(s_search_dl);
                char err[128];
                snprintf(err, sizeof(err), "%s", dl_err(s_search_dl));
                dl_abort(s_search_dl);
                s_search_dl = NULL;
                post_count = 0;
                cursor = 0;
                snprintf(g_status, sizeof(g_status),
                         "fail(%ld) http:%ld %s",
                         code, code, err);
                g_searching = false;
                render_frame();
            }
        }

        thumbs_update(cursor);
        bigview_pump();
        save_pump();
        if (save_take_cam_notice() && !sdata_cam_warn()) {
            sdata_set_cam_warn();
            g_camwarn_open = true;
            sfx_alert();
        }
        sdata_pump(); /* flush settings off the input/critical path */
        bgm_update();
        render_frame();
        gspWaitForVBlank();
    }

    teardown();
    return 0;

fail:
    teardown();
    return 1;
}
