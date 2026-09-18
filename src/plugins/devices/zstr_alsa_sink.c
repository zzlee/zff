/*=============================================================================
    zstr_alsa_sink.c — ALSA Audio Output Sink Device (AVOutputFormat)
=============================================================================*/
#define _GNU_SOURCE
#include "zff/plugins/zstr_alsa_sink.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

typedef struct ALSASinkContext {
    const AVClass *av_class;

    char *device;
    int is_mock;

    int sample_rate;
    int channels;
    enum AVSampleFormat av_sample_fmt;
    snd_pcm_format_t alsa_sample_fmt;
    int bytes_per_sample;
    int frame_bytes;

    snd_pcm_t *handle;
    int64_t samples_written;
    int64_t bytes_written;
} ALSASinkContext;

#define OFFSET(x) offsetof(ALSASinkContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_alsa_sink_options[] = {
    { "device",  "ALSA playback device (e.g. default, hw:0,0)", OFFSET(device),  AV_OPT_TYPE_STRING, { .str = "default" }, 0, 0, ENC },
    { "is_mock", "Force synthetic mock sink",                   OFFSET(is_mock), AV_OPT_TYPE_BOOL,   { .i64 = 0 },         0, 1, ENC },
    { NULL }
};

static const AVClass zstr_alsa_sink_class = {
    .class_name = "zstr_alsa_sink",
    .item_name  = av_default_item_name,
    .option     = zstr_alsa_sink_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int alsa_sink_write_header(AVFormatContext *s) {
    ALSASinkContext *ctx = s->priv_data;

    if (s->nb_streams < 1 || s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(s, AV_LOG_ERROR, "zstr_alsa_sink requires at least one audio stream\n");
        return AVERROR(EINVAL);
    }

    AVCodecParameters *par = s->streams[0]->codecpar;
    ctx->sample_rate = par->sample_rate > 0 ? par->sample_rate : 48000;
    ctx->channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 2;
    ctx->av_sample_fmt = par->format != AV_SAMPLE_FMT_NONE ? par->format : AV_SAMPLE_FMT_S16;

    if (ctx->av_sample_fmt == AV_SAMPLE_FMT_FLT) {
        ctx->alsa_sample_fmt = SND_PCM_FORMAT_FLOAT_LE;
        ctx->bytes_per_sample = sizeof(float);
    } else if (ctx->av_sample_fmt == AV_SAMPLE_FMT_S32) {
        ctx->alsa_sample_fmt = SND_PCM_FORMAT_S32_LE;
        ctx->bytes_per_sample = sizeof(int32_t);
    } else {
        ctx->av_sample_fmt = AV_SAMPLE_FMT_S16;
        ctx->alsa_sample_fmt = SND_PCM_FORMAT_S16_LE;
        ctx->bytes_per_sample = sizeof(int16_t);
    }
    ctx->frame_bytes = ctx->channels * ctx->bytes_per_sample;
    ctx->samples_written = 0;
    ctx->bytes_written = 0;
    ctx->handle = NULL;

    if (!ctx->is_mock) {
        const char *dev_path = (s->url && s->url[0] && strcmp(s->url, "dummy") != 0) ? s->url : ctx->device;
        int err = snd_pcm_open(&ctx->handle, dev_path, SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            ctx->is_mock = 1;
        } else {
            err = snd_pcm_set_params(ctx->handle,
                                     ctx->alsa_sample_fmt,
                                     SND_PCM_ACCESS_RW_INTERLEAVED,
                                     ctx->channels,
                                     ctx->sample_rate,
                                     1, // soft resample
                                     50000); // 50ms latency
            if (err < 0) {
                snd_pcm_close(ctx->handle);
                ctx->handle = NULL;
                ctx->is_mock = 1;
            } else {
                snd_pcm_prepare(ctx->handle);
            }
        }
    }

    return 0;
}

static int alsa_sink_write_packet(AVFormatContext *s, AVPacket *pkt) {
    ALSASinkContext *ctx = s->priv_data;
    if (!pkt || !pkt->data || pkt->size <= 0) return 0;

    int nb_samples = pkt->size / ctx->frame_bytes;
    if (nb_samples <= 0) return 0;

    if (!ctx->is_mock && ctx->handle) {
        snd_pcm_sframes_t written = snd_pcm_writei(ctx->handle, pkt->data, nb_samples);
        if (written < 0) {
            if (written == -EPIPE) {
                snd_pcm_prepare(ctx->handle);
                snd_pcm_writei(ctx->handle, pkt->data, nb_samples);
            }
        }
    }

    ctx->samples_written += nb_samples;
    ctx->bytes_written += pkt->size;
    return 0;
}

static int alsa_sink_write_trailer(AVFormatContext *s) {
    ALSASinkContext *ctx = s->priv_data;
    if (ctx->handle) {
        snd_pcm_drain(ctx->handle);
        snd_pcm_close(ctx->handle);
        ctx->handle = NULL;
    }
    return 0;
}

const FFOutputFormat ff_zstr_alsa_sink_muxer = {
    .p = {
        .name           = "zstr_alsa_sink",
        .long_name      = "zff ALSA Audio Output Sink Device",
        .audio_codec    = AV_CODEC_ID_PCM_S16LE,
        .video_codec    = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE,
        .priv_class     = &zstr_alsa_sink_class,
    },
    .priv_data_size = sizeof(ALSASinkContext),
    .write_header   = alsa_sink_write_header,
    .write_packet   = alsa_sink_write_packet,
    .write_trailer  = alsa_sink_write_trailer,
};
