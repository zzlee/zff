/*=============================================================================
    zstr_audiotestsrc.c — Synthetic Audio Test Signal Generator for FFmpeg
=============================================================================*/
#include "zff/plugins/zstr_audiotestsrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersrc.h>

#define OFFSET(x) offsetof(zstr_audiotestsrc_t, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption zstr_audiotestsrc_options[] = {
    { "sample_rate",       "Sample rate",             OFFSET(sample_rate),       AV_OPT_TYPE_INT,        { .i64 = 48000 }, 8000, 192000, FLAGS },
    { "r",                 "Sample rate",             OFFSET(sample_rate),       AV_OPT_TYPE_INT,        { .i64 = 48000 }, 8000, 192000, FLAGS },
    { "channels",          "Number of channels",      OFFSET(channels),          AV_OPT_TYPE_INT,        { .i64 = 2 }, 1, 8, FLAGS },
    { "c",                 "Number of channels",      OFFSET(channels),          AV_OPT_TYPE_INT,        { .i64 = 2 }, 1, 8, FLAGS },
    { "sample_fmt",        "Sample format",           OFFSET(sample_fmt),        AV_OPT_TYPE_SAMPLE_FMT, { .i64 = AV_SAMPLE_FMT_S16 }, 0, INT_MAX, FLAGS },
    { "wave",              "Waveform type",           OFFSET(wave),              AV_OPT_TYPE_INT,        { .i64 = ZSTR_AUDIO_WAVE_SINE }, 0, 4, FLAGS, "wave" },
        { "sine",          "Sine wave",         0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_AUDIO_WAVE_SINE },        0, 0, FLAGS, "wave" },
        { "square",        "Square wave",       0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_AUDIO_WAVE_SQUARE },      0, 0, FLAGS, "wave" },
        { "white_noise",   "White noise",       0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_AUDIO_WAVE_WHITE_NOISE }, 0, 0, FLAGS, "wave" },
        { "pink_noise",    "Pink noise",        0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_AUDIO_WAVE_PINK_NOISE },  0, 0, FLAGS, "wave" },
        { "silence",       "Silence",           0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_AUDIO_WAVE_SILENCE },     0, 0, FLAGS, "wave" },
    { "freq",              "Tone frequency (Hz)",     OFFSET(frequency),         AV_OPT_TYPE_DOUBLE,     { .dbl = 1000.0 }, 20.0, 20000.0, FLAGS },
    { "f",                 "Tone frequency (Hz)",     OFFSET(frequency),         AV_OPT_TYPE_DOUBLE,     { .dbl = 1000.0 }, 20.0, 20000.0, FLAGS },
    { "volume",            "Volume level (0.0-1.0)",  OFFSET(volume),            AV_OPT_TYPE_DOUBLE,     { .dbl = 0.5 }, 0.0, 1.0, FLAGS },
    { "v",                 "Volume level (0.0-1.0)",  OFFSET(volume),            AV_OPT_TYPE_DOUBLE,     { .dbl = 0.5 }, 0.0, 1.0, FLAGS },
    { "samples_per_frame", "Samples per buffer",      OFFSET(samples_per_frame), AV_OPT_TYPE_INT,        { .i64 = 1024 }, 64, 8192, FLAGS },
    { "num_samples",       "Max samples to output",   OFFSET(num_samples),       AV_OPT_TYPE_INT64,      { .i64 = 0 }, 0, INT64_MAX, FLAGS },
    { NULL }
};

static const AVClass zstr_audiotestsrc_class = {
    .class_name = "zstr_audiotestsrc",
    .item_name  = av_default_item_name,
    .option     = zstr_audiotestsrc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static double generate_sample(zstr_audiotestsrc_t *s) {
    double val = 0.0;
    switch (s->wave) {
        case ZSTR_AUDIO_WAVE_SINE:
            val = sin(2.0 * M_PI * s->phase);
            s->phase += s->frequency / (double)s->sample_rate;
            if (s->phase >= 1.0) s->phase -= 1.0;
            break;
        case ZSTR_AUDIO_WAVE_SQUARE:
            val = (s->phase < 0.5) ? 1.0 : -1.0;
            s->phase += s->frequency / (double)s->sample_rate;
            if (s->phase >= 1.0) s->phase -= 1.0;
            break;
        case ZSTR_AUDIO_WAVE_WHITE_NOISE:
            val = ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;
            break;
        case ZSTR_AUDIO_WAVE_PINK_NOISE: {
            double white = ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;
            s->pink_b0 = 0.99886 * s->pink_b0 + white * 0.0555179;
            s->pink_b1 = 0.99332 * s->pink_b1 + white * 0.0750759;
            s->pink_b2 = 0.96900 * s->pink_b2 + white * 0.1538520;
            s->pink_b3 = 0.86650 * s->pink_b3 + white * 0.3104856;
            s->pink_b4 = 0.55000 * s->pink_b4 + white * 0.5329522;
            s->pink_b5 = -0.7616 * s->pink_b5 - white * 0.0168980;
            val = (s->pink_b0 + s->pink_b1 + s->pink_b2 + s->pink_b3 +
                   s->pink_b4 + s->pink_b5 + s->pink_b6 + white * 0.5362) * 0.11;
            s->pink_b6 = white * 0.115926;
            break;
        }
        case ZSTR_AUDIO_WAVE_SILENCE:
        default:
            val = 0.0;
            break;
    }
    return val * s->volume;
}

zstr_audiotestsrc_t* zstr_audiotestsrc_alloc(const char *opt_string) {
    zstr_audiotestsrc_t *s = av_mallocz(sizeof(zstr_audiotestsrc_t));
    if (!s) return NULL;

    s->av_class = &zstr_audiotestsrc_class;
    av_opt_set_defaults(s);

    if (opt_string && *opt_string) {
        if (av_set_options_string(s, opt_string, "=", ":") < 0) {
            zstr_audiotestsrc_free(&s);
            return NULL;
        }
    }

    av_channel_layout_default(&s->ch_layout, s->channels);
    return s;
}

int zstr_audiotestsrc_read_frame(zstr_audiotestsrc_t *s, AVFrame *frame) {
    if (!s || !frame) return AVERROR(EINVAL);

    if (s->num_samples > 0 && s->total_samples_sent >= s->num_samples) {
        return AVERROR_EOF;
    }

    int nb_samples = s->samples_per_frame;
    if (s->num_samples > 0 && s->total_samples_sent + nb_samples > s->num_samples) {
        nb_samples = (int)(s->num_samples - s->total_samples_sent);
    }

    frame->nb_samples = nb_samples;
    frame->format = s->sample_fmt;
    frame->sample_rate = s->sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &s->ch_layout);
    frame->pts = s->total_samples_sent;

    int ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) return ret;

    ret = av_frame_make_writable(frame);
    if (ret < 0) return ret;

    int is_planar = av_sample_fmt_is_planar(s->sample_fmt);

    for (int i = 0; i < nb_samples; i++) {
        double sample_val = generate_sample(s);

        for (int ch = 0; ch < s->channels; ch++) {
            if (s->sample_fmt == AV_SAMPLE_FMT_S16 || s->sample_fmt == AV_SAMPLE_FMT_S16P) {
                int16_t v16 = (int16_t)round(sample_val * 32767.0);
                if (is_planar) {
                    ((int16_t*)frame->data[ch])[i] = v16;
                } else {
                    ((int16_t*)frame->data[0])[i * s->channels + ch] = v16;
                }
            } else if (s->sample_fmt == AV_SAMPLE_FMT_S32 || s->sample_fmt == AV_SAMPLE_FMT_S32P) {
                int32_t v32 = (int32_t)round(sample_val * 2147483647.0);
                if (is_planar) {
                    ((int32_t*)frame->data[ch])[i] = v32;
                } else {
                    ((int32_t*)frame->data[0])[i * s->channels + ch] = v32;
                }
            } else if (s->sample_fmt == AV_SAMPLE_FMT_FLT || s->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                float vf = (float)sample_val;
                if (is_planar) {
                    ((float*)frame->data[ch])[i] = vf;
                } else {
                    ((float*)frame->data[0])[i * s->channels + ch] = vf;
                }
            }
        }
    }

    s->total_samples_sent += nb_samples;
    s->frame_count++;
    return 0;
}

int zstr_audiotestsrc_attach_to_graph(zstr_audiotestsrc_t *s,
                                      AVFilterGraph *graph,
                                      AVFilterContext **out_src_ctx) {
    if (!s || !graph || !out_src_ctx) return AVERROR(EINVAL);

    const AVFilter *abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) return AVERROR_FILTER_NOT_FOUND;

    char ch_layout_str[128] = {0};
    av_channel_layout_describe(&s->ch_layout, ch_layout_str, sizeof(ch_layout_str));

    char args[512];
    snprintf(args, sizeof(args),
             "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             s->sample_rate, s->sample_rate,
             av_get_sample_fmt_name(s->sample_fmt),
             ch_layout_str);

    int ret = avfilter_graph_create_filter(out_src_ctx, abuffer,
                                          "zstr_audiotestsrc", args, NULL, graph);
    return ret;
}

void zstr_audiotestsrc_free(zstr_audiotestsrc_t **s) {
    if (s && *s) {
        av_opt_free(*s);
        av_channel_layout_uninit(&(*s)->ch_layout);
        av_freep(s);
    }
}
