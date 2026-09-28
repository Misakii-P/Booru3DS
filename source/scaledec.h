#pragma once

/* Pick the largest libjpeg-turbo scale factor whose output still fits
   maxW x maxH, and report the result.
 *
 * Returns false when no factor fits. That happens whenever the source is
   taller than maxH*8 or wider than maxW*8, because 1/8 is the most
 * aggressive factor turbojpeg offers. Callers must treat false as "cannot
   display this", never as "use 1/1": with num/den still 1/1 the computed
 * dw/dh are the full image, and decoding that into a screen-sized buffer
   is a heap overrun of orders of magnitude. It is not hypothetical -
   safebooru serves originals up to 10660x6000.
 *
 * Shared by thumbs, the big view and the camera encoder, which all had
 * their own copy of this and had already drifted apart.
 */
typedef struct { int num, denom; } ScaleFactor;

/* turbojpeg's tjGetScalingFactors(), largest first. Keeping it here means
   the host-side test harness exercises the same table. */
static const ScaleFactor kScaleFactors[] = {
    { 1, 1 }, { 7, 8 }, { 6, 8 }, { 5, 8 },
    { 4, 8 }, { 3, 8 }, { 2, 8 }, { 1, 8 }
};
#define SCALE_FACTOR_COUNT ((int)(sizeof(kScaleFactors) / sizeof(kScaleFactors[0])))

static inline bool scale_fit(int w, int h, int maxW, int maxH,
                             int *outW, int *outH)
{
    int num = 1, den = 1;
    for (int i = 0; i < SCALE_FACTOR_COUNT; i++) {
        const ScaleFactor *sf = &kScaleFactors[i];
        if (sf->num > sf->denom)
            continue;
        int dw = (w * sf->num + sf->denom - 1) / sf->denom;
        int dh = (h * sf->num + sf->denom - 1) / sf->denom;
        if (dw <= maxW && dh <= maxH) {
            num = sf->num;
            den = sf->denom;
            break;
        }
    }
    int dw = (w * num + den - 1) / den;
    int dh = (h * num + den - 1) / den;
    *outW = dw;
    *outH = dh;
    return (dw <= maxW && dh <= maxH);
}
