/* BGM: streaming looper. Supports OGG (via stb_vorbis) and WAV fallback. */
#include "bgm.h"
#include <stdio.h>
#include <string.h>
#include <3ds.h>

typedef struct stb_vorbis stb_vorbis;
typedef struct { unsigned int sample_rate; int channels; unsigned int setup_memory_required; unsigned int temp_memory_required; unsigned int max_frame_size; } stb_vorbis_info;
extern stb_vorbis *stb_vorbis_open_filename(const char *filename, int *error, const void *alloc);
extern void stb_vorbis_close(stb_vorbis *f);
extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
extern int stb_vorbis_seek_start(stb_vorbis *f);

#define NUM_BUFFERS    4
#define SAMPLESPERBUF  0x2000
#define BGM_VOLUME     0.7f

static FILE *s_file;
static bool s_playing;
static bool s_ndsp_ok;
static bool s_on = true;
static bool s_is_ogg = false;
static struct stb_vorbis *s_vorbis;
static u32 s_totalRead;
static u32 s_dataSize;
static u32 s_dataOffset;
static u32 s_sampleRate;
static u16 s_channels;
static ndspWaveBuf s_waveBufs[NUM_BUFFERS];
static s16 *s_bufs[NUM_BUFFERS];

static bool parse_wav_header(FILE *f)
{
    char id[4];
    u32 chunkSize;
    fseek(f, 0, SEEK_SET);
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) != 0) return false;
    if (fseek(f, 4, SEEK_CUR) != 0) return false;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4) != 0) return false;
    bool foundFmt = false, foundData = false;
    while (!foundData) {
        if (fread(id, 1, 4, f) != 4) break;
        if (fread(&chunkSize, 4, 1, f) != 1) break;
        long chunkStart = ftell(f);
        if (memcmp(id, "fmt ", 4) == 0) {
            u16 audioFmt;
            if (fread(&audioFmt, 2, 1, f) != 1 || fread(&s_channels, 2, 1, f) != 1 || fread(&s_sampleRate, 4, 1, f) != 1) break;
            foundFmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            s_dataSize = chunkSize;
            s_dataOffset = (u32)ftell(f);
            foundData = true;
        }
        fseek(f, chunkStart + chunkSize, SEEK_SET);
    }
    return foundFmt && foundData && s_channels >= 1 && s_channels <= 2;
}

static void fill_buffer_wav(int idx)
{
    u32 bytesPerFrame = s_channels * 2;
    u32 bufBytes = SAMPLESPERBUF * bytesPerFrame;
    u32 remaining = s_dataSize - s_totalRead;
    u32 bytesToRead = remaining < bufBytes ? remaining : bufBytes;
    memset(s_bufs[idx], 0, bufBytes);
    size_t read = fread(s_bufs[idx], 1, bytesToRead, s_file);
    s_totalRead += (u32)read;
    DSP_FlushDataCache(s_bufs[idx], bufBytes);
    memset(&s_waveBufs[idx], 0, sizeof(ndspWaveBuf));
    s_waveBufs[idx].data_vaddr = s_bufs[idx];
    s_waveBufs[idx].nsamples = bytesToRead / bytesPerFrame;
    if (s_waveBufs[idx].nsamples == 0) s_waveBufs[idx].nsamples = 1;
    s_waveBufs[idx].status = NDSP_WBUF_DONE;
}

static void fill_buffer_ogg(int idx)
{
    u32 bufBytes = SAMPLESPERBUF * s_channels * 2;
    memset(s_bufs[idx], 0, bufBytes);
    int total = 0;
    int want = SAMPLESPERBUF;
    while (total < want) {
        int n = stb_vorbis_get_samples_short_interleaved(s_vorbis, s_channels, s_bufs[idx] + total * s_channels, (want - total) * s_channels);
        if (n == 0) {
            stb_vorbis_seek_start(s_vorbis);
            n = stb_vorbis_get_samples_short_interleaved(s_vorbis, s_channels, s_bufs[idx] + total * s_channels, (want - total) * s_channels);
            if (n == 0) break;
        }
        total += n;
    }
    DSP_FlushDataCache(s_bufs[idx], bufBytes);
    memset(&s_waveBufs[idx], 0, sizeof(ndspWaveBuf));
    s_waveBufs[idx].data_vaddr = s_bufs[idx];
    s_waveBufs[idx].nsamples = total > 0 ? (u32)total : 1;
    s_waveBufs[idx].status = NDSP_WBUF_DONE;
}

static void fill_buffer(int idx)
{
    if (s_is_ogg) fill_buffer_ogg(idx);
    else fill_buffer_wav(idx);
}

void bgm_init(void)
{
    s_file = NULL;
    s_playing = false;
    s_ndsp_ok = false;
    s_vorbis = NULL;
    s_is_ogg = false;
    if (ndspInit() != 0) return;
    s_ndsp_ok = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetMasterVol(1.0f);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        s_bufs[i] = (s16 *)linearAlloc(SAMPLESPERBUF * 4);
        if (!s_bufs[i]) {
            for (int j = 0; j < i; j++) { linearFree(s_bufs[j]); s_bufs[j] = NULL; }
            s_ndsp_ok = false;
            return;
        }
    }
}

void bgm_exit(void)
{
    bgm_stop();
    for (int i = 0; i < NUM_BUFFERS; i++) if (s_bufs[i]) { linearFree(s_bufs[i]); s_bufs[i] = NULL; }
    if (s_vorbis) { stb_vorbis_close(s_vorbis); s_vorbis = NULL; }
    if (s_ndsp_ok) ndspExit();
}

bool bgm_play(const char *path)
{
    if (!s_ndsp_ok) return false;
    bgm_stop();
    const char *candidates[3];
    int candCount = 0;
    if (strstr(path, ".ogg")) { candidates[candCount++] = path; candidates[candCount++] = "romfs:/bgm.wav"; }
    else { candidates[candCount++] = path; }
    // Also try ogg variant if wav requested
    char oggAlt[256];
    if (strstr(path, ".wav")) {
        snprintf(oggAlt, sizeof(oggAlt), "%s", path);
        char *dot = strrchr(oggAlt, '.');
        if (dot) strcpy(dot, ".ogg");
        bool dup = false;
        for (int i = 0; i < candCount; i++) if (!strcmp(candidates[i], oggAlt)) dup = true;
        if (!dup && candCount < 3) candidates[candCount++] = oggAlt;
    }

    for (int ci = 0; ci < candCount; ci++) {
        const char *p = candidates[ci];
        bool useOgg = strstr(p, ".ogg") != NULL;
        if (useOgg) {
            int err = 0;
            s_vorbis = stb_vorbis_open_filename(p, &err, NULL);
            if (!s_vorbis) continue;
            stb_vorbis_info info = stb_vorbis_get_info(s_vorbis);
            s_channels = info.channels;
            s_sampleRate = info.sample_rate;
            if (s_channels < 1 || s_channels > 2 || s_sampleRate == 0) { stb_vorbis_close(s_vorbis); s_vorbis = NULL; continue; }
            s_is_ogg = true;
            s_file = NULL;
            s_totalRead = 0;
            s_dataSize = 0;
            break;
        } else {
            s_file = fopen(p, "rb");
            if (!s_file) continue;
            if (!parse_wav_header(s_file)) { fclose(s_file); s_file = NULL; continue; }
            s_is_ogg = false;
            break;
        }
    }
    if (!s_is_ogg && !s_file) return false;

    u16 format = (s_channels == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
    ndspChnReset(0);
    ndspChnWaveBufClear(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)s_sampleRate);
    ndspChnSetFormat(0, format);
    float mix[12] = {0};
    mix[0] = BGM_VOLUME; mix[1] = BGM_VOLUME;
    ndspChnSetMix(0, mix);
    s_totalRead = 0;
    memset(s_waveBufs, 0, sizeof(s_waveBufs));
    for (int i = 0; i < NUM_BUFFERS; i++) {
        fill_buffer(i);
        ndspChnWaveBufAdd(0, &s_waveBufs[i]);
    }
    s_playing = true;
    return true;
}

void bgm_stop(void)
{
    if (s_file) { fclose(s_file); s_file = NULL; }
    if (s_vorbis) { stb_vorbis_close(s_vorbis); s_vorbis = NULL; }
    s_is_ogg = false;
    if (s_playing) { ndspChnWaveBufClear(0); s_playing = false; }
}

void bgm_set_on(bool on)
{
    if (!s_ndsp_ok || on == s_on) return;
    s_on = on;
    if (!on) {
        ndspChnWaveBufClear(0);
        for (int i = 0; i < NUM_BUFFERS; i++) s_waveBufs[i].status = NDSP_WBUF_DONE;
    }
}

bool bgm_on(void) { return s_on; }

void bgm_update(void)
{
    if (!s_playing || !s_on) return;
    if (!s_is_ogg && !s_file) return;
    if (s_is_ogg && !s_vorbis) return;
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (s_waveBufs[i].status == NDSP_WBUF_DONE) {
            if (!s_is_ogg) {
                if (s_totalRead >= s_dataSize) { fseek(s_file, s_dataOffset, SEEK_SET); s_totalRead = 0; }
                u32 remaining = s_dataSize - s_totalRead;
                if (remaining == 0) continue;
            }
            fill_buffer(i);
            ndspChnWaveBufAdd(0, &s_waveBufs[i]);
        }
    }
}
