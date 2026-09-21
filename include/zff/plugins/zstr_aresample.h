/*=============================================================================
    zstr_aresample.h — Audio Resampler with ASRC Drift Compensation
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZSTR_ASRC_MODE_NONE = 0,
    ZSTR_ASRC_MODE_PTS  = 1
} zstr_asrc_mode_t;

typedef struct zstr_aresample zstr_aresample_t;

/**
 * Allocate and initialize a new zstr_aresample instance.
 *
 * @param opt_string Key=value configuration string.
 *                   e.g. "out_sample_rate=48000:out_channels=2:asrc_mode=pts:max_drift_ppm=1000"
 * @return Allocated instance, or NULL on error.
 */
zstr_aresample_t* zstr_aresample_alloc(const char *opt_string);

/**
 * Process and resample an input audio AVFrame into an output AVFrame.
 * Handles dynamic input format changes automatically.
 *
 * PTS convention: input `pts` is in NANOSECONDS (<= 0 means unknown, in
 * which case a sample-counter continuation is used). Output `pts` is
 * likewise in nanoseconds, predicted via swr_next_pts() so filter
 * delay and active ASRC/fractional compensation are accounted for.
 *
 * Fractional override: when rate_numer/rate_denom are set, swr runs at the
 * rounded integer rate and swr_set_compensation() fine-tunes the exact
 * ratio before every conversion.
 *
 * @param s   Pointer to zstr_aresample_t.
 * @param in  Input audio AVFrame.
 * @param out Output audio AVFrame (allocated/reallocated as necessary).
 * @return 0 on success, negative AVERROR on failure.
 */
int zstr_aresample_process(zstr_aresample_t *s, const AVFrame *in, AVFrame *out);

/**
 * Drain samples buffered inside swr (filter delay + compensation tail).
 * Output pts continues seamlessly from the last process() call.
 * Emits at most one frame per call; call repeatedly until
 * out->nb_samples == 0.
 *
 * @param s   Pointer to zstr_aresample_t.
 * @param out Output audio AVFrame (caller-allocated).
 * @return 0 on success, negative AVERROR on failure.
 */
int zstr_aresample_flush(zstr_aresample_t *s, AVFrame *out);

/**
 * Free zstr_aresample instance and underlying SwrContext.
 */
void zstr_aresample_free(zstr_aresample_t **s);

#ifdef __cplusplus
}
#endif
