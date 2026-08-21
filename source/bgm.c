/*
 * Background music: streaming WAV looper on ndsp channel 0.
 * Double-buffered, seeks back to data start when exhausted.
 */

#include "bgm.h"

#include <stdio.h>
#include <string.h>

#include <3ds.h>

/* 4 buffers x 8192 samples ~= 750ms of buffered audio: long blocking
   operations (image decode/save) must not drain the pipeline */
#define NUM_BUFFERS    4
#define SAMPLESPERBUF  0x2000
#define BGM_VOLUME     0.7f

static FILE *s_file;
static bool s_playing;
static bool s_ndsp_ok;
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
                fread(&s_channels, 2, 1, f) != 1 ||
                fread(&s_sampleRate, 4, 1, f) != 1)
                break;
            foundFmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            s_dataSize = chunkSize;
            s_dataOffset = (u32)ftell(f);
            foundData = true;
        }

        /* skip unknown chunks (LIST etc.) */
        fseek(f, chunkStart + chunkSize, SEEK_SET);
    }

    return foundFmt && foundData && s_channels >= 1 && s_channels <= 2;
}

static void fill_buffer(int idx)
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
    /* only submit the frames actually read (clean loop seam) */
    s_waveBufs[idx].nsamples = bytesToRead / bytesPerFrame;
    if (s_waveBufs[idx].nsamples == 0)
        s_waveBufs[idx].nsamples = 1;
    s_waveBufs[idx].status = NDSP_WBUF_DONE;
}

void bgm_init(void)
{
    s_file = NULL;
    s_playing = false;
    s_ndsp_ok = false;

    if (ndspInit() != 0)
        return;
    s_ndsp_ok = true;

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetMasterVol(1.0f);

    for (int i = 0; i < NUM_BUFFERS; i++)
        s_bufs[i] = (s16 *)linearAlloc(SAMPLESPERBUF * 4);
}

void bgm_exit(void)
{
    bgm_stop();
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (s_bufs[i]) {
            linearFree(s_bufs[i]);
            s_bufs[i] = NULL;
        }
    }
    ndspExit();
}

bool bgm_play(const char *path)
{
    if (!s_ndsp_ok)
        return false;

    bgm_stop();

    s_file = fopen(path, "rb");
    if (!s_file)
        return false;

    if (!parse_wav_header(s_file)) {
        fclose(s_file);
        s_file = NULL;
        return false;
    }

    u16 format = (s_channels == 2) ? NDSP_FORMAT_STEREO_PCM16
                                   : NDSP_FORMAT_MONO_PCM16;

    ndspChnReset(0);
    ndspChnWaveBufClear(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)s_sampleRate);
    ndspChnSetFormat(0, format);

    float mix[12] = {0};
    mix[0] = BGM_VOLUME;
    mix[1] = BGM_VOLUME;
    ndspChnSetMix(0, mix);

    s_totalRead = 0;
    memset(s_waveBufs, 0, sizeof(s_waveBufs));

    fseek(s_file, s_dataOffset, SEEK_SET);
    fill_buffer(0);
    fill_buffer(1);

    ndspChnWaveBufAdd(0, &s_waveBufs[0]);
    ndspChnWaveBufAdd(0, &s_waveBufs[1]);

    s_playing = true;
    return true;
}

void bgm_stop(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    if (s_playing) {
        ndspChnWaveBufClear(0);
        s_playing = false;
    }
}

void bgm_update(void)
{
    if (!s_playing || !s_file)
        return;

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (s_waveBufs[i].status == NDSP_WBUF_DONE) {
            if (s_totalRead >= s_dataSize) {
                /* loop: back to data start */
                fseek(s_file, s_dataOffset, SEEK_SET);
                s_totalRead = 0;
            }

            u32 remaining = s_dataSize - s_totalRead;
            if (remaining == 0)
                continue;

            fill_buffer(i);
            ndspChnWaveBufAdd(0, &s_waveBufs[i]);
        }
    }
}
