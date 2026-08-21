#pragma once

typedef enum { SCR_HOME, SCR_LIST } Screen;

/* bottom screen layout (shared by ui.c rendering + main.c touch) */
#define BOT_W       320
#define HOME_BTN_X  4
#define HOME_BTN_Y  4
#define HOME_BTN_S  36
#define SBAR_LX     48
#define SBAR_TY     6
#define SBAR_RX     316
#define SBAR_BY     38
#define GRID_X      0
#define GRID_Y      46
#define CELL_W      80
#define CELL_H      64
#define GRID_COLS   4
#define GRID_ROWS   3
#define PAGE_SIZE   (GRID_COLS * GRID_ROWS)

extern int cursor;
extern char current_tags[128];
extern Screen screen;
extern char g_status[128];
extern unsigned long g_res, g_http, g_size;
extern const char *g_provider_name;
extern int g_provider; /* index into provider list */
extern bool g_save_prompt; /* save-destination dialog open */
