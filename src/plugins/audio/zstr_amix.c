/*=============================================================================
    zstr_amix.c — Audio Multi-Channel Mixer with Soft-Clipping Limiter
=============================================================================*/
#include "zff/plugins/zstr_amix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

typedef struct {
    double volume;
    double pan;  /* -1.0 left to 1.0 right */
    bool mute;

    /* Resampler for format matching */
    SwrContext *swr;
    int in_rate;
    enum AVSampleFormat in_fmt;
    AVChannelLayout in_layout;
} amix_slot_t;

struct zstr_amix {
    int nb_inputs;
    int sample_rate;
    int channels;
    enum AVSampleFormat out_sample_fmt;
    AVChannelLayout out_layout;
    int normalize;

    amix_slot_t slots[ZSTR_AMIX_MAX_INPUTS];

    /* Accumulator buffer */
    double *fmix;
    size_t fmix_capacity;
};

static inline double soft_clip(double x) {
    if (x > 1.0) {
        return (x > 3.0) ? 1.0 : tanhf((float)x);
    } else if (x < -1.0) {
        return (x < -3.0) ? -1.0 : tanhf((float)x);
    }
    return x;
}

zstr_amix_t* zstr_amix_alloc(const char *opt_string) {
    zstr_amix_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->nb_inputs = 2;
    m->sample_rate = 48000;
    m->channels = 2;
    m->out_sample_fmt = AV_SAMPLE_FMT_FLT;
    m->normalize = 0;
    av_channel_layout_default(&m->out_layout, m->channels);

    for (int i = 0; i < ZSTR_AMIX_MAX_INPUTS; i++) {
        m->slots[i].volume = 1.0;
        m->slots[i].pan = 0.0;
        m->slots[i].mute = false;
        m->slots[i].swr = NULL;
    }

    if (opt_string && opt_string[0] != '\0') {
        char *copy = strdup(opt_string);
        if (copy) {
            char *token = strtok(copy, ":,");
            while (token) {
                char *eq = strchr(token, '=');
                if (eq) {
                    *eq = '\0';
                    const char *key = token;
                    const char *val = eq + 1;

                    if (strcmp(key, "inputs") == 0) {
                        int n = atoi(val);
                        if (n > 0 && n <= ZSTR_AMIX_MAX_INPUTS) m->nb_inputs = n;
                    } else if (strcmp(key, "sample_rate") == 0 || strcmp(key, "rate") == 0) {
                        int r = atoi(val);
                        if (r > 0) m->sample_rate = r;
                    } else if (strcmp(key, "channels") == 0) {
                        int c = atoi(val);
                        if (c > 0) {
                            m->channels = c;
                            av_channel_layout_uninit(&m->out_layout);
                            av_channel_layout_default(&m->out_layout, m->channels);
                        }
                    } else if (strcmp(key, "sample_fmt") == 0 || strcmp(key, "format") == 0) {
                        if (strcmp(val, "s16") == 0) m->out_sample_fmt = AV_SAMPLE_FMT_S16;
                        else if (strcmp(val, "s32") == 0) m->out_sample_fmt = AV_SAMPLE_FMT_S32;
                        else m->out_sample_fmt = AV_SAMPLE_FMT_FLT;
                    } else if (strcmp(key, "normalize") == 0) {
                        m->normalize = atoi(val);
                    } else if (strcmp(key, "weights") == 0) {
                        char *w_copy = strdup(val);
                        if (w_copy) {
                            char *w_tok = strtok(w_copy, "|;");
                            int idx = 0;
                            while (w_tok && idx < m->nb_inputs) {
                                m->slots[idx].volume = atof(w_tok);
                                idx++;
                                w_tok = strtok(NULL, "|;");
                            }
                            free(w_copy);
                        }
                    }
                }
                token = strtok(NULL, ":,");
            }
            free(copy);
        }
    }

    return m;
}

int zstr_amix_set_input_volume(zstr_amix_t *m, int input_idx, double volume) {
    if (!m || input_idx < 0 || input_idx >= m->nb_inputs) return AVERROR(EINVAL);
    m->slots[input_idx].volume = volume;
    return 0;
}

int zstr_amix_set_input_mute(zstr_amix_t *m, int input_idx, bool mute) {
    if (!m || input_idx < 0 || input_idx >= m->nb_inputs) return AVERROR(EINVAL);
    m->slots[input_idx].mute = mute;
    return 0;
}

int zstr_amix_set_input_pan(zstr_amix_t *m, int input_idx, double pan) {
    if (!m || input_idx < 0 || input_idx >= m->nb_inputs) return AVERROR(EINVAL);
    m->slots[input_idx].pan = pan;
    return 0;
}

static AVFrame* resample_input_if_needed(zstr_amix_t *m, int idx, const AVFrame *in) {
    amix_slot_t *slot = &m->slots[idx];

    /* Check if input matches master configuration */
    if (in->sample_rate == m->sample_rate &&
        in->format == m->out_sample_fmt &&
        in->ch_layout.nb_channels == m->channels) {
        return (AVFrame *)in; /* Direct match */
    }

    /* Check if SwrContext needs recreation */
    if (!slot->swr ||
        slot->in_rate != in->sample_rate ||
        slot->in_fmt != (enum AVSampleFormat)in->format ||
        av_channel_layout_compare(&slot->in_layout, &in->ch_layout) != 0) {

        if (slot->swr) swr_free(&slot->swr);
        av_channel_layout_uninit(&slot->in_layout);
        av_channel_layout_copy(&slot->in_layout, &in->ch_layout);

        slot->in_rate = in->sample_rate;
        slot->in_fmt = in->format;

        int ret = swr_alloc_set_opts2(
            &slot->swr,
            &m->out_layout,
            m->out_sample_fmt,
            m->sample_rate,
            &in->ch_layout,
            (enum AVSampleFormat)in->format,
            in->sample_rate,
            0, NULL
        );
        if (ret < 0 || !slot->swr || swr_init(slot->swr) < 0) {
            return NULL;
        }
    }

    int out_samples = av_rescale_rnd(
        swr_get_delay(slot->swr, in->sample_rate) + in->nb_samples,
        m->sample_rate, in->sample_rate, AV_ROUND_UP
    );

    AVFrame *conv = av_frame_alloc();
    if (!conv) return NULL;

    conv->sample_rate = m->sample_rate;
    conv->format = m->out_sample_fmt;
    av_channel_layout_copy(&conv->ch_layout, &m->out_layout);
    conv->nb_samples = out_samples;

    if (av_frame_get_buffer(conv, 0) < 0) {
        av_frame_free(&conv);
        return NULL;
    }

    int ret_samples = swr_convert(
        slot->swr,
        conv->extended_data,
        out_samples,
        (const uint8_t **)in->extended_data,
        in->nb_samples
    );
    if (ret_samples < 0) {
        av_frame_free(&conv);
        return NULL;
    }

    conv->nb_samples = ret_samples;
    conv->pts = in->pts;
    return conv;
}

int zstr_amix_mix(zstr_amix_t *m, const AVFrame * const *in, int nb_in, AVFrame *out) {
    if (!m || !in || nb_in <= 0 || !out) {
        return AVERROR(EINVAL);
    }

    int actual_inputs = nb_in < m->nb_inputs ? nb_in : m->nb_inputs;

    /* Resample each input if needed */
    AVFrame *frames[ZSTR_AMIX_MAX_INPUTS] = {0};
    int max_samples = 0;
    int active_inputs = 0;

    for (int i = 0; i < actual_inputs; i++) {
        if (!in[i] || m->slots[i].mute) continue;

        frames[i] = resample_input_if_needed(m, i, in[i]);
        if (frames[i] && frames[i]->nb_samples > 0) {
            if (frames[i]->nb_samples > max_samples) {
                max_samples = frames[i]->nb_samples;
            }
            active_inputs++;
        }
    }

    if (max_samples <= 0) {
        /* Clean up temporary resampled frames */
        for (int i = 0; i < actual_inputs; i++) {
            if (frames[i] && frames[i] != in[i]) av_frame_free(&frames[i]);
        }
        return AVERROR(EINVAL);
    }

    /* Resize accumulator buffer if necessary */
    size_t required_capacity = (size_t)max_samples * m->channels;
    if (m->fmix_capacity < required_capacity) {
        double *new_fmix = realloc(m->fmix, required_capacity * sizeof(double));
        if (!new_fmix) {
            for (int i = 0; i < actual_inputs; i++) {
                if (frames[i] && frames[i] != in[i]) av_frame_free(&frames[i]);
            }
            return AVERROR(ENOMEM);
        }
        m->fmix = new_fmix;
        m->fmix_capacity = required_capacity;
    }
    memset(m->fmix, 0, required_capacity * sizeof(double));

    /* Accumulate samples */
    for (int i = 0; i < actual_inputs; i++) {
        AVFrame *f = frames[i];
        if (!f) continue;

        double volume = m->slots[i].volume;
        double pan = m->slots[i].pan;

        double g0 = volume;
        double g1 = volume;
        if (m->channels >= 2) {
            g0 *= (pan <= 0.0 ? 1.0 : (1.0 - pan));
            g1 *= (pan >= 0.0 ? 1.0 : (1.0 + pan));
        }

        int f_samples = f->nb_samples;
        int in_ch = f->ch_layout.nb_channels;

        if (f->format == AV_SAMPLE_FMT_FLT) {
            const float *src = (const float *)f->data[0];
            for (int s = 0; s < f_samples; s++) {
                if (in_ch >= 2 && m->channels >= 2) {
                    m->fmix[s * 2 + 0] += (double)src[s * in_ch + 0] * g0;
                    m->fmix[s * 2 + 1] += (double)src[s * in_ch + 1] * g1;
                } else if (in_ch == 1 && m->channels >= 2) {
                    double v = (double)src[s];
                    m->fmix[s * 2 + 0] += v * g0;
                    m->fmix[s * 2 + 1] += v * g1;
                } else {
                    m->fmix[s * m->channels + 0] += (double)src[s * in_ch + 0] * g0;
                }
            }
        } else if (f->format == AV_SAMPLE_FMT_S16) {
            const int16_t *src = (const int16_t *)f->data[0];
            for (int s = 0; s < f_samples; s++) {
                if (in_ch >= 2 && m->channels >= 2) {
                    m->fmix[s * 2 + 0] += ((double)src[s * in_ch + 0] / 32768.0) * g0;
                    m->fmix[s * 2 + 1] += ((double)src[s * in_ch + 1] / 32768.0) * g1;
                } else if (in_ch == 1 && m->channels >= 2) {
                    double v = (double)src[s] / 32768.0;
                    m->fmix[s * 2 + 0] += v * g0;
                    m->fmix[s * 2 + 1] += v * g1;
                } else {
                    m->fmix[s * m->channels + 0] += ((double)src[s * in_ch + 0] / 32768.0) * g0;
                }
            }
        } else if (f->format == AV_SAMPLE_FMT_S32) {
            const int32_t *src = (const int32_t *)f->data[0];
            for (int s = 0; s < f_samples; s++) {
                if (in_ch >= 2 && m->channels >= 2) {
                    m->fmix[s * 2 + 0] += ((double)src[s * in_ch + 0] / 2147483648.0) * g0;
                    m->fmix[s * 2 + 1] += ((double)src[s * in_ch + 1] / 2147483648.0) * g1;
                } else {
                    m->fmix[s * m->channels + 0] += ((double)src[s * in_ch + 0] / 2147483648.0) * g0;
                }
            }
        }
    }

    /* Prepare output frame */
    av_frame_unref(out);
    out->sample_rate = m->sample_rate;
    out->format = m->out_sample_fmt;
    av_channel_layout_copy(&out->ch_layout, &m->out_layout);
    out->nb_samples = max_samples;

    int ret = av_frame_get_buffer(out, 0);
    if (ret < 0) {
        for (int i = 0; i < actual_inputs; i++) {
            if (frames[i] && frames[i] != in[i]) av_frame_free(&frames[i]);
        }
        return ret;
    }

    /* Normalization or Soft-clipping Limiter */
    double norm_scale = (m->normalize && active_inputs > 1) ? (1.0 / active_inputs) : 1.0;
    size_t total_samples = (size_t)max_samples * m->channels;

    if (m->out_sample_fmt == AV_SAMPLE_FMT_FLT) {
        float *dst = (float *)out->data[0];
        for (size_t j = 0; j < total_samples; j++) {
            double v = m->fmix[j] * norm_scale;
            if (!m->normalize) v = soft_clip(v);
            dst[j] = (float)v;
        }
    } else if (m->out_sample_fmt == AV_SAMPLE_FMT_S16) {
        int16_t *dst = (int16_t *)out->data[0];
        for (size_t j = 0; j < total_samples; j++) {
            double v = m->fmix[j] * norm_scale;
            if (!m->normalize) v = soft_clip(v);
            if (v > 1.0) v = 1.0;
            if (v < -1.0) v = -1.0;
            dst[j] = (int16_t)(v * 32767.0);
        }
    } else if (m->out_sample_fmt == AV_SAMPLE_FMT_S32) {
        int32_t *dst = (int32_t *)out->data[0];
        for (size_t j = 0; j < total_samples; j++) {
            double v = m->fmix[j] * norm_scale;
            if (!m->normalize) v = soft_clip(v);
            if (v > 1.0) v = 1.0;
            if (v < -1.0) v = -1.0;
            dst[j] = (int32_t)(v * 2147483647.0);
        }
    }

    /* Metadata propagation */
    if (in[0]) {
        out->pts = in[0]->pts;
        out->duration = max_samples;
        av_frame_copy_props(out, in[0]);
    }

    /* Clean up temporary resampled frames */
    for (int i = 0; i < actual_inputs; i++) {
        if (frames[i] && frames[i] != in[i]) {
            av_frame_free(&frames[i]);
        }
    }

    return 0;
}

void zstr_amix_free(zstr_amix_t **m) {
    if (!m || !*m) return;
    zstr_amix_t *inst = *m;

    for (int i = 0; i < ZSTR_AMIX_MAX_INPUTS; i++) {
        if (inst->slots[i].swr) {
            swr_free(&inst->slots[i].swr);
        }
        av_channel_layout_uninit(&inst->slots[i].in_layout);
    }
    av_channel_layout_uninit(&inst->out_layout);

    if (inst->fmix) {
        free(inst->fmix);
        inst->fmix = NULL;
    }

    free(inst);
    *m = NULL;
}
