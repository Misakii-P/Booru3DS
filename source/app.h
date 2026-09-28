#pragma once

typedef enum { SCR_HOME, SCR_LIST } Screen;

/* bottom screen layout (shared by ui.c rendering + main.c touch) */
#define BOT_W       320
#define HOME_BTN_X  4
#define HOME_BTN_Y  4
#define HOME_BTN_S  36
#define SBAR_LX     48
#define SBAR_TY     6
#define SBAR_RX     268
#define SBAR_BY     38
#define GRID_X      0
#define GRID_Y      46
#define CELL_W      80
#define CELL_H      64
#define GRID_COLS   4
#define GRID_ROWS   3
#define PAGE_SIZE   (GRID_COLS * GRID_ROWS)

/* history button, bottom-left corner */
#define HIST_BTN_X  4
#define HIST_BTN_Y  194
#define HIST_BTN_S  40

/* about button, bottom-right corner */
#define ABOUT_BTN_X 276
#define ABOUT_BTN_Y 194
#define ABOUT_BTN_S 40

/* sound toggle, top-right corner */
#define SND_BTN_X   280
#define SND_BTN_Y   4
#define SND_BTN_S   36

extern int cursor;
extern char current_tags[128];
extern Screen screen;
extern char g_status[256];
extern int g_provider; /* index into the provider table in net.h */
extern bool g_save_prompt; /* save-destination dialog open */
extern bool g_hist_open;   /* search-history overlay open */
extern int g_hist_sel;     /* highlighted history row */
extern bool g_about_open;  /* about popup open */
extern bool g_camwarn_open; /* first camera-install notice */
extern bool g_searching;   /* a search request is in flight */
