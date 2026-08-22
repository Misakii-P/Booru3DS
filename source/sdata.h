#pragma once

#define SDATA_HIST_MAX 8

void sdata_load(void);
void sdata_save(void);

int sdata_provider(void);
void sdata_set_provider(int pv);

/* most recent first */
int sdata_hist_count(void);
const char *sdata_hist_get(int i);
void sdata_hist_push(const char *tags);

bool sdata_cam_warn(void);   /* first camera-install warning shown? */
void sdata_set_cam_warn(void);
