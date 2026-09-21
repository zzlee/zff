/*=============================================================================
    zstr_amix.h — Audio Multi-Channel Mixer with Soft-Clipping Limiter
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZSTR_AMIX_MAX_INPUTS 32

typedef struct zstr_amix zstr_amix_t;

/**
 * Allocate and initialize a zstr_amix instance.
 *
 * @param opt_string Key=value configuration string.
 *                   e.g. "inputs=2:sample_rate=48000:channels=2:sample_fmt=flt:normalize=0:weights=1.0|0.8"
 *                   Options:
 *                     - inputs: number of inputs (default 2, up to 32)
 *                     - sample_rate, rate: output master sample rate (default 48000)
 *                     - channels: output master channels (default 2)
 *                     - sample_fmt, format: output format ("s16", "s32", "flt", default "flt")
 *                     - normalize: 1 = divide by active inputs, 0 = soft-clipping limiter (default 0)
 *                     - weights: pipe-separated volumes (e.g. "1.0|0.8")
 * @return Allocated instance, or NULL on error.
 */
zstr_amix_t* zstr_amix_alloc(const char *opt_string);

/**
 * Dynamically configure runtime parameters via key=value string.
 * Supported options:
 *   - "volume@<idx>=<val>" (e.g. "volume@0=0.5")
 *   - "mute@<idx>=<0|1>"   (e.g. "mute@1=1")
 *   - "pan@<idx>=<val>"    (e.g. "pan@0=-0.5")
 *   - "normalize=<0|1>"
 *
 * Multiple options can be separated by ':' or ','.
 */
int zstr_amix_set_param(zstr_amix_t *m, const char *param_str);

/**
 * Direct synchronous frame processing:
 * Mixes multiple input AVFrames directly into one output AVFrame.
 * Automatically performs sample rate/format conversion and soft-clipping limiter.
 *
 * @param m       Pointer to zstr_amix_t.
 * @param in      Array of const AVFrame* pointers (size nb_in).
 * @param nb_in   Number of input frames in array.
 * @param out     Output AVFrame.
 * @return 0 on success, negative AVERROR on error.
 */
int zstr_amix_process(zstr_amix_t *m, const AVFrame * const *in, int nb_in, AVFrame *out);

/**
 * Free zstr_amix instance and resources.
 */
void zstr_amix_free(zstr_amix_t **m);

/* ── Backward Compatibility Helper Wrappers ────────────────────────────── */
static inline int zstr_amix_mix(zstr_amix_t *m, const AVFrame * const *in, int nb_in, AVFrame *out) {
    return zstr_amix_process(m, in, nb_in, out);
}

#ifdef __cplusplus
}
#endif
