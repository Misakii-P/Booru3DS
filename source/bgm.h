#pragma once

#include <stdbool.h>

void bgm_init(void);
void bgm_exit(void);
bool bgm_play(const char *path); /* wav file, loops forever */
void bgm_stop(void);
void bgm_update(void);           /* call once per frame */
