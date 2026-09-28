#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <3ds.h>

#include "sfx.h"

#define SFX_VOLUME 0.9f
#define SFX_COUNT  2 /* click, alert */

typedef struct
{
    s16 *data;
    u32 frames;
    u32 rate;
    u16 channels;
    bool ok;
    ndspWaveBuf wb;
} SfxVoice;

static SfxVoice s_v[SFX_COUNT];
static const char *const SFX_PATHS[SFX_COUNT] = {
    "romfs:/click.wav",
    "romfs:/alert.wav",
};
static const int SFX_CH[SFX_COUNT] = { 1, 2 };

static bool parse_header(FILE *f, long *dataOff, u32 *dataBytes,
                         u32 *rate, u16 *channels)
{
    char id[4];
    u32 chunkSize;

    fseek(f, 0, SEEK_SET);
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) != 0)
        return false;
    if (fseek(f, 4, SEEK_CUR) != 0)
        return false;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4) != 0)
        return false;

    bool foundFmt = false, foundData = false;
    while (!foundData) {
        if (fread(id, 1, 4, f) != 4)
            break;
        if (fread(&chunkSize, 4, 1, f) != 1)
            break;
        long chunkStart = ftell(f);

        if (memcmp(id, "fmt ", 4) == 0) {
            u16 audioFmt;
            if (fread(&audioFmt, 2, 1, f) != 1 ||
                fread(channels, 2, 1, f) != 1 ||
                fread(rate, 4, 1, f) != 1)
                break;
            foundFmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            *dataOff = chunkStart;
            *dataBytes = chunkSize;
            foundData = true;
        }

        fseek(f, chunkStart + chunkSize, SEEK_SET);
    }

    return foundFmt && foundData && *dataBytes > 0 &&
           *channels >= 1 && *channels <= 2;
}

/* per-channel mixer is the only volume control available (no
   ndspChnSetVol in libctru); zeroing it is instant */
static void apply_mix(int ch)
{
    float mix[12] = { 0 };
    mix[0] = SFX_VOLUME;
    mix[1] = SFX_VOLUME;
    ndspChnSetMix(ch, mix);
}

static void load_voice(int i)
{
    SfxVoice *v = &s_v[i];

    FILE *f = fopen(SFX_PATHS[i], "rb");
    if (!f)
        return;

    long off = 0;
    u32 bytes = 0;
    if (!parse_header(f, &off, &bytes, &v->rate, &v->channels)) {
        fclose(f);
        return;
    }
    v->frames = bytes / (v->channels * 2);
    if (v->frames == 0) {
        fclose(f);
        return;
    }

    v->data = (s16 *)linearAlloc(bytes);
    if (!v->data) {
        fclose(f);
        return;
    }
    if (fseek(f, off, SEEK_SET) != 0 ||
        fread(v->data, 1, bytes, f) != bytes) {
        linearFree(v->data);
        v->data = NULL;
        fclose(f);
        return;
    }
    fclose(f);

    DSP_FlushDataCache(v->data, bytes);

    ndspChnReset(SFX_CH[i]);
    ndspChnSetInterp(SFX_CH[i], NDSP_INTERP_LINEAR);
    ndspChnSetRate(SFX_CH[i], (float)v->rate);
    ndspChnSetFormat(SFX_CH[i],
                     v->channels == 2 ? NDSP_FORMAT_STEREO_PCM16
                                      : NDSP_FORMAT_MONO_PCM16);
    apply_mix(SFX_CH[i]);

    memset(&v->wb, 0, sizeof(v->wb));
    v->wb.data_vaddr = v->data;
    v->wb.nsamples = v->frames;
    v->wb.status = NDSP_WBUF_DONE;

    v->ok = true;
}

void sfx_init(void)
{
    for (int i = 0; i < SFX_COUNT; i++)
        load_voice(i);
}

void sfx_exit(void)
{
    for (int i = 0; i < SFX_COUNT; i++) {
        if (s_v[i].ok)
            ndspChnWaveBufClear(SFX_CH[i]);
        if (s_v[i].data) {
            linearFree(s_v[i].data);
            s_v[i].data = NULL;
        }
        s_v[i].ok = false;
    }
}

static void play(int i)
{
    SfxVoice *v = &s_v[i];
    if (!v->ok)
        return;
    /* also restores the mix if a suspend silenced the channel */
    apply_mix(SFX_CH[i]);
    ndspChnWaveBufClear(SFX_CH[i]); /* restart cleanly on rapid triggers */
    v->wb.status = NDSP_WBUF_DONE;
    ndspChnWaveBufAdd(SFX_CH[i], &v->wb);
}

void sfx_click(void) { play(0); }
void sfx_alert(void) { play(1); }

/* Same suspend/resume hazard as the BGM: the ndsp queue is dropped by the
   console but our cached waveBuf is not, so a queued effect comes back
   stale. play() re-adds on the next trigger, we just have to make sure
   the cached buffer is clean and its status says "ready" again. */
void sfx_suspend(void)
{
    float zero[12] = { 0 };
    for (int i = 0; i < SFX_COUNT; i++) {
        if (!s_v[i].ok) continue;
        /* silence before dropping the queue, for the same reason as the BGM:
           a half-played effect is still in our cache and would be audible */
        ndspChnSetMix(SFX_CH[i], zero);
        ndspChnWaveBufClear(SFX_CH[i]);
    }
}

void sfx_resume(void)
{
    for (int i = 0; i < SFX_COUNT; i++) {
        SfxVoice *v = &s_v[i];
        if (!v->data) continue;
        DSP_FlushDataCache(v->data, v->frames * v->channels * 2);
        v->wb.status = NDSP_WBUF_DONE;
    }
    /* mix stays zeroed; play() restores it on the next trigger */
}
