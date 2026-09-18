/*=============================================================================
    zstr_audiotestsrc.h — Synthetic Audio Test Signal Generator for FFmpeg
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavfilter/avfilter.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZSTR_AUDIO_WAVE_SINE,
    ZSTR_AUDIO_WAVE_SQUARE,
    ZSTR_AUDIO_WAVE_WHITE_NOISE,
    ZSTR_AUDIO_WAVE_PINK_NOISE,
    ZSTR_AUDIO_WAVE_SILENCE
} zstr_audio_wave_t;

typedef struct zstr_audiotestsrc {
    const AVClass *av_class;

    int sample_rate;
    int channels;
    AVChannelLayout ch_layout;
    enum AVSampleFormat sample_fmt;
    int wave;
    double frequency;
    double volume;
    int samples_per_frame;

    int64_t num_samples;
    int64_t total_samples_sent;
    int64_t frame_count;

    double phase;
    uint32_t rng_state;

    /* Pink noise filter state (Paul Kellet 7-pole approximation) */
    double pink_b0, pink_b1, pink_b2, pink_b3, pink_b4, pink_b5, pink_b6;
} zstr_audiotestsrc_t;

/**
 * Allocate and initialize a new audio test source instance.
 *
 * @param opt_string Optional key=value string (e.g. "r=48000:c=2:wave=sine:f=1000:v=0.5").
 * @return Allocated instance, or NULL on error.
 */
zstr_audiotestsrc_t* zstr_audiotestsrc_alloc(const char *opt_string);

/**
 * Read the next generated audio frame.
 *
 * @param s     Pointer to zstr_audiotestsrc_t.
 * @param frame Pre-allocated AVFrame to populate.
 * @return 0 on success, AVERROR_EOF if num_samples reached, or negative on error.
 */
int zstr_audiotestsrc_read_frame(zstr_audiotestsrc_t *s, AVFrame *frame);

/**
 * Convenience helper: Attach this audio test source to an AVFilterGraph as an abuffer node.
 *
 * @param s           Valid zstr_audiotestsrc_t.
 * @param graph       Valid AVFilterGraph.
 * @param out_src_ctx Output pointer to created abuffer AVFilterContext.
 * @return 0 on success, negative on error.
 */
int zstr_audiotestsrc_attach_to_graph(zstr_audiotestsrc_t *s,
                                      AVFilterGraph *graph,
                                      AVFilterContext **out_src_ctx);

/**
 * Free audio test source instance.
 */
void zstr_audiotestsrc_free(zstr_audiotestsrc_t **s);

#ifdef __cplusplus
}
#endif
