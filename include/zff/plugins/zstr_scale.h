/*=============================================================================
    zstr_scale.h — Video Scaler with SIMD 64-byte Alignment & Dynamic Reconfiguration
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zstr_scale zstr_scale_t;

/**
 * Allocate and initialize a zstr_scale instance.
 *
 * @param opt_string Key=value configuration string.
 *                   e.g. "w=1920:h=1080:format=yuv420p:flags=bilinear:align=64"
 *                   Options:
 *                     - w, width: target width (0 = keep input width)
 *                     - h, height: target height (0 = keep input height)
 *                     - format, pix_fmt: target pixel format name (e.g. "yuv420p", "nv12", "rgb24")
 *                     - flags: "bilinear", "bicubic", "fast_bilinear", "neighbor"
 *                     - align: output stride alignment in bytes, power of 2 (default 64).
 *                       Kept for downstream HW stride requirements; no measured
 *                       scaler speedup vs 1/32 on AVX2 (swscale issue, not stride-bound).
 *                   The input color_range is preserved (full-range content is
 *                   NOT remapped to limited).
 * @return Allocated instance, or NULL on error.
 */
zstr_scale_t* zstr_scale_alloc(const char *opt_string);

/**
 * Process and scale an input video AVFrame into an output AVFrame.
 * Handles dynamic input resolution, format, and colorspace changes automatically.
 * Supports zero-copy passthrough if format, dimensions, and alignment match.
 * Propagates PTS, durations, and side data (including PTP / Hardware FourCC tags).
 *
 * @param s   Pointer to zstr_scale_t.
 * @param in  Input video AVFrame.
 * @param out Output video AVFrame (allocated/reallocated as necessary with specified alignment).
 * @return 0 on success, negative AVERROR on failure.
 */
int zstr_scale_process(zstr_scale_t *s, const AVFrame *in, AVFrame *out);

/**
 * Free zstr_scale instance and underlying SwsContext.
 */
void zstr_scale_free(zstr_scale_t **s);

#ifdef __cplusplus
}
#endif
