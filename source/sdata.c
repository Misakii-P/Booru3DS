#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "sdata.h"

#define SDATA_PATH "sdmc:/3ds/booru/save.dat"
#define SDATA_TAGS 128

static struct
{
    int provider;
    bool camwarn;
    int hist_n;
    char hist[SDATA_HIST_MAX][SDATA_TAGS];
} s;

void sdata_load(void)
{
    memset(&s, 0, sizeof(s));

    FILE *f = fopen(SDATA_PATH, "r");
    if (!f)
        return;

    char line[SDATA_TAGS + 16];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "provider=", 9))
            s.provider = atoi(line + 9);
        else if (!strncmp(line, "camwarn=", 8))
            s.camwarn = atoi(line + 8) != 0;
        else if (!strncmp(line, "hist=", 5) && s.hist_n < SDATA_HIST_MAX) {
            size_t vl = strlen(line + 5);
            if (vl > SDATA_TAGS - 1)
                vl = SDATA_TAGS - 1;
            memcpy(s.hist[s.hist_n], line + 5, vl);
            s.hist[s.hist_n][vl] = 0;
            s.hist_n++;
        }
    }
    fclose(f);

    /* clamp to a valid provider index (crash-proof against corrupt saves) */
    if (s.provider < 0 || s.provider > 15)
        s.provider = 0;
}

void sdata_save(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/booru", 0777);

    FILE *f = fopen(SDATA_PATH, "w");
    if (!f)
        return;
    fprintf(f, "provider=%d\n", s.provider);
    fprintf(f, "camwarn=%d\n", s.camwarn ? 1 : 0);
    for (int i = 0; i < s.hist_n; i++)
        fprintf(f, "hist=%s\n", s.hist[i]);
    fclose(f);
}

bool sdata_cam_warn(void)
{
    return s.camwarn;
}

void sdata_set_cam_warn(void)
{
    if (s.camwarn)
        return;
    s.camwarn = true;
    sdata_save();
}

int sdata_provider(void)
{
    return s.provider;
}

void sdata_set_provider(int pv)
{
    if (pv == s.provider)
        return;
    s.provider = pv;
    sdata_save();
}

int sdata_hist_count(void)
{
    return s.hist_n;
}

const char *sdata_hist_get(int i)
{
    if (i < 0 || i >= s.hist_n)
        return NULL;
    return s.hist[i];
}

void sdata_hist_push(const char *tags)
{
    if (!tags || !tags[0])
        return;

    /* move existing duplicate to front */
    int found = -1;
    for (int i = 0; i < s.hist_n; i++) {
        if (!strcmp(s.hist[i], tags)) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        for (int i = found; i > 0; i--)
            strncpy(s.hist[i], s.hist[i - 1], SDATA_TAGS);
        strncpy(s.hist[0], tags, SDATA_TAGS - 1);
        s.hist[0][SDATA_TAGS - 1] = 0;
        sdata_save();
        return;
    }

    /* shift down, cap at 10 */
    if (s.hist_n < SDATA_HIST_MAX)
        s.hist_n++;
    for (int i = s.hist_n - 1; i > 0; i--)
        memcpy(s.hist[i], s.hist[i - 1], sizeof(s.hist[0]));
    memset(s.hist[0], 0, sizeof(s.hist[0]));
    strncpy(s.hist[0], tags, SDATA_TAGS - 1);

    sdata_save();
}
