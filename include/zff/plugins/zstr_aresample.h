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
 * @param s   Pointer to zstr_aresample_t.
 * @param in  Input audio AVFrame.
 * @param out Output audio AVFrame (allocated/reallocated as necessary).
 * @return 0 on success, negative AVERROR on failure.
 */
int zstr_aresample_process(zstr_aresample_t *s, const AVFrame *in, AVFrame *out);

/**
 * Free zstr_aresample instance and underlying SwrContext.
 */
void zstr_aresample_free(zstr_aresample_t **s);

#ifdef __cplusplus
}
#endif
