#pragma once

#include <stdbool.h>

void bgm_init(void);
void bgm_exit(void);
bool bgm_play(const char *path); /* wav file, loops forever */
void bgm_stop(void);
void bgm_update(void);           /* call once per frame */
void bgm_set_on(bool on);        /* mute/unmute bgm only (channel 0) */
bool bgm_on(void);

/* APT suspend/resume. The ndsp queue is lost across a suspend while our
   waveBuf.status fields are not, so these have to be called from an APT
   hook or playback comes back distorted. */
void bgm_suspend(void);
void bgm_resume(void);
