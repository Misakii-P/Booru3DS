#pragma once

/* ui sound effects on dedicated ndsp channels - independent from bgm mute.
   click = ch1, alert = ch2 */

void sfx_init(void);   /* after ndsp is up (bgm_init) */
void sfx_exit(void);
void sfx_click(void);
void sfx_alert(void);
