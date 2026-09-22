/*=============================================================================
    zstr_aresample.c — Audio Resampler with ASRC Drift Compensation
=============================================================================*/
#include "zff/plugins/zstr_aresample.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#define NANOS_PER_SEC 1000000000LL

struct zstr_aresample {
    const AVClass *av_class;

    int out_sample_rate;
    int out_channels;
    AVChannelLayout out_ch_layout;
    enum AVSampleFormat out_sample_fmt;

    int asrc_mode;
    double max_drift_ppm;
    int drift_interval;
    int rate_numer;
    int rate_denom;
    int block_samples;

    /* Current input tracking for dynamic format switches */
    struct SwrContext *swr;
    int cur_in_rate;
    AVChannelLayout cur_in_ch_layout;
    enum AVSampleFormat cur_in_fmt;
    /* Output parameters actually programmed at last reinit (reinit when
     * the live options drift from these, e.g. via set_param) */
    int applied_out_rate;
    enum AVSampleFormat applied_out_fmt;
    AVChannelLayout applied_out_ch_layout;
    int swr_out_rate;      /* integer rate actually programmed into swr
                              (may be nudged +1 or rounded from fractional) */
    int comp_delta;        /* fractional-rate compensation delta (0 = none) */
    int comp_dist;         /* fractional-rate compensation distance */

    /* ASRC state */
    int64_t last_in_pts;
    double cum_drift_output;
    int buf_count_since_check;
    int drift_adjust_count;

    int64_t total_in_samples;
    int64_t total_out_samples;
    int64_t next_pts_ns;   /* expected pts (ns) of next output frame */
};

#define OFFSET(x) offsetof(struct zstr_aresample, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption zstr_aresample_options[] = {
    { "out_sample_rate", "Target sample rate (Hz)", OFFSET(out_sample_rate), AV_OPT_TYPE_INT,        { .i64 = 48000 }, 8000, 192000, FLAGS },
    { "r",               "Target sample rate (Hz)", OFFSET(out_sample_rate), AV_OPT_TYPE_INT,        { .i64 = 48000 }, 8000, 192000, FLAGS },
    { "out_channels",    "Target audio channels",   OFFSET(out_channels),    AV_OPT_TYPE_INT,        { .i64 = 2 },     1, 8, FLAGS },
    { "c",               "Target audio channels",   OFFSET(out_channels),    AV_OPT_TYPE_INT,        { .i64 = 2 },     1, 8, FLAGS },
    { "out_sample_fmt",  "Target sample format",    OFFSET(out_sample_fmt),  AV_OPT_TYPE_SAMPLE_FMT, { .i64 = AV_SAMPLE_FMT_S16 }, 0, INT_MAX, FLAGS },
    { "asrc_mode",       "ASRC drift mode",         OFFSET(asrc_mode),       AV_OPT_TYPE_INT,        { .i64 = ZSTR_ASRC_MODE_NONE }, 0, 1, FLAGS, "asrc_mode" },
        { "none",        "No drift compensation",   0, AV_OPT_TYPE_CONST,   { .i64 = ZSTR_ASRC_MODE_NONE }, 0, 0, FLAGS, "asrc_mode" },
        { "pts",         "PTS-based compensation",  0, AV_OPT_TYPE_CONST,   { .i64 = ZSTR_ASRC_MODE_PTS },  0, 0, FLAGS, "asrc_mode" },
    { "max_drift_ppm",   "Max drift in PPM",        OFFSET(max_drift_ppm),   AV_OPT_TYPE_DOUBLE,     { .dbl = 1000.0 }, 10.0, 10000.0, FLAGS },
    { "drift_interval",  "Buffers between checks",  OFFSET(drift_interval),  AV_OPT_TYPE_INT,        { .i64 = 4 },     1, 100, FLAGS },
    { "rate_numer",      "Target rate numerator",   OFFSET(rate_numer),      AV_OPT_TYPE_INT,        { .i64 = 0 },     0, INT_MAX, FLAGS },
    { "rate_denom",      "Target rate denominator", OFFSET(rate_denom),      AV_OPT_TYPE_INT,        { .i64 = 0 },     0, INT_MAX, FLAGS },
    { "block_samples",   "Output block size",       OFFSET(block_samples),   AV_OPT_TYPE_INT,        { .i64 = 0 },     0, 8192, FLAGS },
    { NULL }
};

static const AVClass zstr_aresample_class = {
    .class_name = "zstr_aresample",
    .item_name  = av_default_item_name,
    .option     = zstr_aresample_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int reinit_swr(zstr_aresample_t *s, const AVFrame *in) {
    if (s->swr) {
        swr_free(&s->swr);
    }
    /* Reset ASRC/PTS tracking on reconfiguration (mirrors zstreamer) */
    s->last_in_pts = 0;
    s->cum_drift_output = 0.0;
    s->buf_count_since_check = 0;
    s->next_pts_ns = AV_NOPTS_VALUE;
    s->comp_delta = 0;
    s->comp_dist = 0;

    av_channel_layout_uninit(&s->cur_in_ch_layout);
    av_channel_layout_copy(&s->cur_in_ch_layout, &in->ch_layout);
    s->cur_in_rate = in->sample_rate;
    s->cur_in_fmt = in->format;

    /* Determine the integer rate programmed into swr.
     * swr ignores swr_set_compensation() on its 1:1 memcpy fast path, so
     * whenever compensation may be applied (ASRC PTS mode or fractional
     * override) we force a true resampling filter by nudging off equality.
     * Fractional targets are rounded for swr and fine-tuned via a
     * pre-computed compensation delta over a long distance. */
    int swr_rate = s->out_sample_rate;
    if (s->rate_numer > 0 && s->rate_denom > 0) {
        double target_rate = (double)s->rate_numer / (double)s->rate_denom;
        swr_rate = (int)(target_rate + 0.5);
        if (swr_rate == in->sample_rate)
            swr_rate += (target_rate > in->sample_rate) ? 1 : -1;
        double actual_ratio = (double)swr_rate / (double)in->sample_rate;
        double target_ratio = target_rate / (double)in->sample_rate;
        int dist = 4800000;
        int delta = (int)((target_ratio - actual_ratio) * (double)dist + 0.5);
        if (delta != 0 && dist > 0) {
            s->comp_delta = delta;
            s->comp_dist = dist;
        }
    } else if (s->asrc_mode == ZSTR_ASRC_MODE_PTS && swr_rate == in->sample_rate) {
        swr_rate += 1;
    }
    s->swr_out_rate = swr_rate;

    int ret = swr_alloc_set_opts2(&s->swr,
                                  &s->out_ch_layout, s->out_sample_fmt, swr_rate,
                                  &s->cur_in_ch_layout, s->cur_in_fmt, s->cur_in_rate,
                                  0, NULL);
    if (ret < 0 || !s->swr) return ret < 0 ? ret : AVERROR(ENOMEM);

    ret = swr_init(s->swr);
    if (ret < 0) return ret;

    /* Snapshot the programmed output geometry */
    s->applied_out_rate = s->out_sample_rate;
    s->applied_out_fmt = s->out_sample_fmt;
    av_channel_layout_uninit(&s->applied_out_ch_layout);
    av_channel_layout_copy(&s->applied_out_ch_layout, &s->out_ch_layout);
    return ret;
}

static double asrc_detect_drift(zstr_aresample_t *s, int64_t pts, int in_samples, int in_rate, int out_rate) {
    if (s->asrc_mode != ZSTR_ASRC_MODE_PTS || pts <= 0) return 0.0;
    if (s->last_in_pts <= 0 || pts <= s->last_in_pts) {
        s->last_in_pts = pts;
        return 0.0;
    }

    int64_t delta_pts = pts - s->last_in_pts;
    s->last_in_pts = pts;

    double expected_in = (double)delta_pts * (double)in_rate / (double)NANOS_PER_SEC;
    if (expected_in < 1.0 || expected_in > (double)in_samples * 10.0) return 0.0;

    double drift_in = (double)in_samples - expected_in;
    double max_plausible = expected_in * s->max_drift_ppm / 1.0e6;
    if (fabs(drift_in) > max_plausible * 2.0) {
        return 0.0; // Discontinuity
    }

    return drift_in * (double)out_rate / (double)in_rate;
}

static void asrc_apply_compensation(zstr_aresample_t *s, int next_out_samples) {
    if (s->asrc_mode != ZSTR_ASRC_MODE_PTS || !s->swr) return;

    s->buf_count_since_check++;
    if (s->buf_count_since_check < s->drift_interval) return;
    s->buf_count_since_check = 0;

    if (fabs(s->cum_drift_output) < 1.0) return;

    int sample_delta = (int)s->cum_drift_output;
    int max_delta = next_out_samples / 4;
    if (max_delta < 1) max_delta = 1;
    if (sample_delta > max_delta) sample_delta = max_delta;
    if (sample_delta < -max_delta) sample_delta = -max_delta;

    if (swr_set_compensation(s->swr, sample_delta, next_out_samples) == 0) {
        s->cum_drift_output -= (double)sample_delta;
        s->drift_adjust_count++;
    }
}

zstr_aresample_t* zstr_aresample_alloc(const char *opt_string) {
    zstr_aresample_t *s = av_mallocz(sizeof(zstr_aresample_t));
    if (!s) return NULL;

    s->av_class = &zstr_aresample_class;
    av_opt_set_defaults(s);

    if (opt_string && *opt_string) {
        if (av_set_options_string(s, opt_string, "=", ":") < 0) {
            zstr_aresample_free(&s);
            return NULL;
        }
    }

    av_channel_layout_default(&s->out_ch_layout, s->out_channels);
    s->next_pts_ns = AV_NOPTS_VALUE;
    return s;
}

/* Runtime reconfiguration (engine contract set_param): option changes
 * take effect on the next process() via the applied-geometry check. */
int zstr_aresample_set_param(zstr_aresample_t *s, const char *param_str) {
    if (!s || !param_str || !*param_str) return AVERROR(EINVAL);
    if (av_set_options_string(s, param_str, "=", ":") < 0) return AVERROR(EINVAL);
    av_channel_layout_uninit(&s->out_ch_layout);
    av_channel_layout_default(&s->out_ch_layout, s->out_channels);
    return 0;
}

int zstr_aresample_process(zstr_aresample_t *s, const AVFrame *in, AVFrame *out) {
    if (!s || !in || !out) return AVERROR(EINVAL);

    /* Check for dynamic format changes on input, or output geometry
     * changes via set_param */
    if (!s->swr ||
        s->cur_in_rate != in->sample_rate ||
        s->cur_in_fmt != in->format ||
        av_channel_layout_compare(&s->cur_in_ch_layout, &in->ch_layout) != 0 ||
        s->applied_out_rate != s->out_sample_rate ||
        s->applied_out_fmt != s->out_sample_fmt ||
        av_channel_layout_compare(&s->applied_out_ch_layout, &s->out_ch_layout) != 0) {

        int ret = reinit_swr(s, in);
        if (ret < 0) return ret;
    }

    /* Effective resampling ratio. With a fractional override the swr
     * context runs at the rounded integer rate, so buffer sizing must use
     * the exact rational ratio (numer / (in_rate * denom)). */
    int resc_out = s->out_sample_rate;
    int resc_in = in->sample_rate;
    if (s->rate_numer > 0 && s->rate_denom > 0) {
        resc_out = s->rate_numer;
        resc_in = in->sample_rate * s->rate_denom;
    }

    /* Calculate output sample count with margin */
    int64_t delay = swr_get_delay(s->swr, in->sample_rate);
    int max_out_samples = (int)av_rescale_rnd(delay + in->nb_samples,
                                              resc_out, resc_in, AV_ROUND_UP);

    /* Setup output frame */
    out->sample_rate = s->out_sample_rate;
    out->format = s->out_sample_fmt;
    av_channel_layout_copy(&out->ch_layout, &s->out_ch_layout);
    out->nb_samples = max_out_samples;

    int ret = av_frame_get_buffer(out, 0);
    if (ret < 0) return ret;

    ret = av_frame_make_writable(out);
    if (ret < 0) return ret;

    /* Re-apply fractional-rate compensation before every convert: a new
     * call restarts the distance counter so the correction never expires. */
    if (s->comp_dist > 0 && s->comp_delta != 0) {
        swr_set_compensation(s->swr, s->comp_delta, s->comp_dist);
    }

    /* ASRC Drift Detection & Compensation */
    if (s->asrc_mode == ZSTR_ASRC_MODE_PTS) {
        double drift = asrc_detect_drift(s, in->pts, in->nb_samples, in->sample_rate, s->out_sample_rate);
        s->cum_drift_output += drift;
        asrc_apply_compensation(s, max_out_samples);
    }

    /* Predict output PTS via swr_next_pts (accounts filter delay +
     * active compensation). Input pts convention is nanoseconds.
     * Falls back to sample-counter continuation when input has no pts. */
    int64_t out_pts_ns;
    if (in->pts > 0) {
        int64_t inpts = av_rescale(in->pts,
                                   (int64_t)s->swr_out_rate * in->sample_rate,
                                   NANOS_PER_SEC);
        int64_t o = swr_next_pts(s->swr, inpts);
        int64_t out_samples = (o + in->sample_rate / 2) / in->sample_rate;
        out_pts_ns = av_rescale(out_samples, NANOS_PER_SEC, s->swr_out_rate);
    } else if (s->next_pts_ns != AV_NOPTS_VALUE) {
        out_pts_ns = s->next_pts_ns;
    } else {
        out_pts_ns = av_rescale(s->total_out_samples, NANOS_PER_SEC, s->swr_out_rate);
    }

    /* Resample conversion */
    int converted = swr_convert(s->swr,
                                out->data, max_out_samples,
                                (const uint8_t**)in->data, in->nb_samples);
    if (converted < 0) return converted;

    out->nb_samples = converted;
    out->pts = out_pts_ns; /* nanoseconds */
    out->time_base = (AVRational){ 1, 1000000000 };
    s->next_pts_ns = out_pts_ns + av_rescale(converted, NANOS_PER_SEC, s->swr_out_rate);
    s->total_in_samples += in->nb_samples;
    s->total_out_samples += converted;

    return 0;
}

/* Drain samples buffered inside swr (filter delay + compensation tail).
 * Emits at most one frame; call repeatedly until out->nb_samples == 0. */
int zstr_aresample_flush(zstr_aresample_t *s, AVFrame *out) {
    if (!s || !out) return AVERROR(EINVAL);
    if (!s->swr) return AVERROR(EINVAL);

    int64_t delay = swr_get_delay(s->swr, s->cur_in_rate);
    int n = (int)av_rescale_rnd(delay, s->swr_out_rate, s->cur_in_rate, AV_ROUND_UP);

    out->sample_rate = s->out_sample_rate;
    out->format = s->out_sample_fmt;
    av_channel_layout_copy(&out->ch_layout, &s->out_ch_layout);
    out->time_base = (AVRational){ 1, 1000000000 };

    if (n <= 0) {
        out->nb_samples = 0;
        out->pts = s->next_pts_ns;
        return 0;
    }

    out->nb_samples = n;
    int ret = av_frame_get_buffer(out, 0);
    if (ret < 0) return ret;
    ret = av_frame_make_writable(out);
    if (ret < 0) return ret;

    int converted = swr_convert(s->swr, out->data, n, NULL, 0);
    if (converted < 0) return converted;

    out->nb_samples = converted;
    out->pts = (s->next_pts_ns != AV_NOPTS_VALUE) ? s->next_pts_ns
             : av_rescale(s->total_out_samples, NANOS_PER_SEC, s->swr_out_rate);
    s->next_pts_ns = out->pts + av_rescale(converted, NANOS_PER_SEC, s->swr_out_rate);
    s->total_out_samples += converted;
    return 0;
}

void zstr_aresample_free(zstr_aresample_t **s) {
    if (s && *s) {
        av_opt_free(*s);
        if ((*s)->swr) {
            swr_free(&(*s)->swr);
        }
        av_channel_layout_uninit(&(*s)->out_ch_layout);
        av_channel_layout_uninit(&(*s)->cur_in_ch_layout);
        av_channel_layout_uninit(&(*s)->applied_out_ch_layout);
        av_freep(s);
    }
}
