/*=============================================================================
    zstr_alsa.c — ALSA Audio Input Device (AVInputFormat)
=============================================================================*/
#define _GNU_SOURCE
#include "zff/plugins/zstr_alsa.h"
#include "zff/zff_core.h"
#include "zff/zff_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#define QUEUE_CAPACITY 32

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct ALSADeviceContext {
    const AVClass *av_class;

    char *device;
    int sample_rate;
    int channels;
    char *sample_fmt;
    int block_size;
    int is_mock;
    int64_t num_samples;
    int realtime;

    /* Resolved audio parameters */
    enum AVSampleFormat av_sample_fmt;
    snd_pcm_format_t alsa_sample_fmt;
    int bytes_per_sample;
    int frame_bytes;

    /* Hardware ALSA state */
    snd_pcm_t *handle;

    /* Background worker thread & FIFO queue */
    pthread_t worker_thread;
    pthread_mutex_t lock;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    int thread_started;
    int stop_requested;
    int eof_reached;

    AVPacket *queue[QUEUE_CAPACITY];
    int q_head;
    int q_tail;
    int q_count;
} ALSADeviceContext;

#define OFFSET(x) offsetof(ALSADeviceContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_alsa_options[] = {
    { "device",      "ALSA capture device name (e.g. default, hw:0,0)", OFFSET(device),      AV_OPT_TYPE_STRING, { .str = "default" }, 0, 0, DEC },
    { "sample_rate", "Audio sample rate",                               OFFSET(sample_rate), AV_OPT_TYPE_INT,    { .i64 = 48000 },     1, 192000, DEC },
    { "channels",    "Number of audio channels",                        OFFSET(channels),    AV_OPT_TYPE_INT,    { .i64 = 2 },         1, 16, DEC },
    { "sample_fmt",  "Sample format (s16, s32, flt)",                   OFFSET(sample_fmt),  AV_OPT_TYPE_STRING, { .str = "s16" },     0, 0, DEC },
    { "block_size",  "Audio frame block size (samples)",                OFFSET(block_size),  AV_OPT_TYPE_INT,    { .i64 = 1024 },      64, 8192, DEC },
    { "is_mock",     "Force synthetic mock fallback",                   OFFSET(is_mock),     AV_OPT_TYPE_BOOL,   { .i64 = 0 },         0, 1, DEC },
    { "realtime",    "Real-time audio pacing (1=on, 0=burst)",          OFFSET(realtime),    AV_OPT_TYPE_BOOL,   { .i64 = 1 },         0, 1, DEC },
    { "num_samples", "Max samples to capture (0=infinite)",             OFFSET(num_samples), AV_OPT_TYPE_INT64,  { .i64 = 0 },         0, INT64_MAX, DEC },
    { NULL }
};

static const AVClass zstr_alsa_class = {
    .class_name = "zstr_alsa",
    .item_name  = av_default_item_name,
    .option     = zstr_alsa_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static void generate_mock_audio(ALSADeviceContext *ctx, uint8_t *data, int nb_samples, int64_t total_samples) {
    double freq = 440.0;
    double sample_rate = (double)ctx->sample_rate;

    if (ctx->av_sample_fmt == AV_SAMPLE_FMT_FLT) {
        float *pcm = (float *)data;
        for (int i = 0; i < nb_samples; i++) {
            float val = (float)sin(2.0 * M_PI * freq * (total_samples + i) / sample_rate) * 0.5f;
            for (int ch = 0; ch < ctx->channels; ch++) {
                pcm[i * ctx->channels + ch] = val;
            }
        }
    } else if (ctx->av_sample_fmt == AV_SAMPLE_FMT_S32) {
        int32_t *pcm = (int32_t *)data;
        for (int i = 0; i < nb_samples; i++) {
            int32_t val = (int32_t)(sin(2.0 * M_PI * freq * (total_samples + i) / sample_rate) * 0x3fffffff);
            for (int ch = 0; ch < ctx->channels; ch++) {
                pcm[i * ctx->channels + ch] = val;
            }
        }
    } else {
        /* S16 */
        int16_t *pcm = (int16_t *)data;
        for (int i = 0; i < nb_samples; i++) {
            int16_t val = (int16_t)(sin(2.0 * M_PI * freq * (total_samples + i) / sample_rate) * 16384.0);
            for (int ch = 0; ch < ctx->channels; ch++) {
                pcm[i * ctx->channels + ch] = val;
            }
        }
    }
}

static void* alsa_worker(void *arg) {
    ALSADeviceContext *ctx = (ALSADeviceContext *)arg;
    int64_t total_samples = 0;

    int64_t interval_ns = (int64_t)1000000000L * ctx->block_size / ctx->sample_rate;
    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

    while (1) {
        pthread_mutex_lock(&ctx->lock);
        if (ctx->stop_requested) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }

        if (ctx->num_samples > 0 && total_samples >= ctx->num_samples) {
            ctx->eof_reached = 1;
            pthread_cond_broadcast(&ctx->cond_not_empty);
            pthread_mutex_unlock(&ctx->lock);
            break;
        }

        while (ctx->q_count == QUEUE_CAPACITY && !ctx->stop_requested) {
            pthread_cond_wait(&ctx->cond_not_full, &ctx->lock);
        }
        if (ctx->stop_requested) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }
        pthread_mutex_unlock(&ctx->lock);

        int cur_samples = ctx->block_size;
        if (ctx->num_samples > 0 && (total_samples + cur_samples) > ctx->num_samples) {
            cur_samples = (int)(ctx->num_samples - total_samples);
        }

        int packet_bytes = cur_samples * ctx->frame_bytes;
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) break;
        if (av_new_packet(pkt, packet_bytes) < 0) {
            av_packet_free(&pkt);
            break;
        }

        if (!ctx->is_mock && ctx->handle) {
            /* Real ALSA hardware read with overrun recovery */
            snd_pcm_sframes_t read_frames = snd_pcm_readi(ctx->handle, pkt->data, cur_samples);
            if (read_frames < 0) {
                if (read_frames == -EPIPE) {
                    /* Buffer underrun/overrun, recover */
                    snd_pcm_prepare(ctx->handle);
                }
                memset(pkt->data, 0, packet_bytes);
            } else if (read_frames < cur_samples) {
                int read_bytes = (int)read_frames * ctx->frame_bytes;
                memset(pkt->data + read_bytes, 0, packet_bytes - read_bytes);
            }
        } else {
            /* Synthetic mock audio generation with real-time cadence */
            if (ctx->realtime) {
                next_time.tv_nsec += interval_ns;
                while (next_time.tv_nsec >= 1000000000L) {
                    next_time.tv_sec += 1;
                    next_time.tv_nsec -= 1000000000L;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
            }

            generate_mock_audio(ctx, pkt->data, cur_samples, total_samples);
        }

        pkt->pts = total_samples;
        pkt->dts = total_samples;
        pkt->duration = cur_samples;
        pkt->stream_index = 0;

        /* Attach FourCC PTP SideData */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        zff_ptp_time_t ptp = {
            .tai_nanoseconds = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec,
            .domain = 0
        };
        zff_packet_set_ptp(pkt, &ptp);

        total_samples += cur_samples;

        pthread_mutex_lock(&ctx->lock);
        ctx->queue[ctx->q_tail] = pkt;
        ctx->q_tail = (ctx->q_tail + 1) % QUEUE_CAPACITY;
        ctx->q_count++;
        pthread_cond_signal(&ctx->cond_not_empty);
        pthread_mutex_unlock(&ctx->lock);
    }

    return NULL;
}

static int alsa_read_header(AVFormatContext *s) {
    ALSADeviceContext *ctx = s->priv_data;

    if (strcmp(ctx->sample_fmt, "flt") == 0) {
        ctx->av_sample_fmt = AV_SAMPLE_FMT_FLT;
        ctx->alsa_sample_fmt = SND_PCM_FORMAT_FLOAT_LE;
        ctx->bytes_per_sample = sizeof(float);
    } else if (strcmp(ctx->sample_fmt, "s32") == 0) {
        ctx->av_sample_fmt = AV_SAMPLE_FMT_S32;
        ctx->alsa_sample_fmt = SND_PCM_FORMAT_S32_LE;
        ctx->bytes_per_sample = sizeof(int32_t);
    } else {
        ctx->av_sample_fmt = AV_SAMPLE_FMT_S16;
        ctx->alsa_sample_fmt = SND_PCM_FORMAT_S16_LE;
        ctx->bytes_per_sample = sizeof(int16_t);
    }
    ctx->frame_bytes = ctx->channels * ctx->bytes_per_sample;

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = AV_CODEC_ID_PCM_S16LE;
    if (ctx->av_sample_fmt == AV_SAMPLE_FMT_FLT) {
        st->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
    } else if (ctx->av_sample_fmt == AV_SAMPLE_FMT_S32) {
        st->codecpar->codec_id = AV_CODEC_ID_PCM_S32LE;
    }

    st->codecpar->sample_rate = ctx->sample_rate;
    av_channel_layout_default(&st->codecpar->ch_layout, ctx->channels);
    st->codecpar->format = ctx->av_sample_fmt;
    st->time_base = (AVRational){ 1, ctx->sample_rate };

    /* Hardware ALSA setup or mock fallback */
    ctx->handle = NULL;
    if (!ctx->is_mock) {
        const char *dev_name = ctx->device ? ctx->device : "default";
        int err = snd_pcm_open(&ctx->handle, dev_name, SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0) {
            ctx->is_mock = 1; /* Fallback to mock */
        } else {
            err = snd_pcm_set_params(ctx->handle,
                                     ctx->alsa_sample_fmt,
                                     SND_PCM_ACCESS_RW_INTERLEAVED,
                                     ctx->channels,
                                     ctx->sample_rate,
                                     1, // soft resample allowed
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

    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond_not_empty, NULL);
    pthread_cond_init(&ctx->cond_not_full, NULL);

    ctx->q_head = 0;
    ctx->q_tail = 0;
    ctx->q_count = 0;
    ctx->stop_requested = 0;
    ctx->eof_reached = 0;

    if (pthread_create(&ctx->worker_thread, NULL, alsa_worker, ctx) != 0) {
        if (ctx->handle) {
            snd_pcm_close(ctx->handle);
            ctx->handle = NULL;
        }
        return AVERROR(EIO);
    }
    ctx->thread_started = 1;
    return 0;
}

static int alsa_read_packet(AVFormatContext *s, AVPacket *pkt) {
    ALSADeviceContext *ctx = s->priv_data;

    pthread_mutex_lock(&ctx->lock);
    while (ctx->q_count == 0) {
        if (ctx->eof_reached || ctx->stop_requested) {
            pthread_mutex_unlock(&ctx->lock);
            return AVERROR_EOF;
        }
        pthread_cond_wait(&ctx->cond_not_empty, &ctx->lock);
    }

    AVPacket *queued = ctx->queue[ctx->q_head];
    ctx->queue[ctx->q_head] = NULL;
    ctx->q_head = (ctx->q_head + 1) % QUEUE_CAPACITY;
    ctx->q_count--;

    pthread_cond_signal(&ctx->cond_not_full);
    pthread_mutex_unlock(&ctx->lock);

    av_packet_move_ref(pkt, queued);
    av_packet_free(&queued);
    return 0;
}

static int alsa_read_close(AVFormatContext *s) {
    ALSADeviceContext *ctx = s->priv_data;
    if (ctx->thread_started) {
        pthread_mutex_lock(&ctx->lock);
        ctx->stop_requested = 1;
        pthread_cond_broadcast(&ctx->cond_not_empty);
        pthread_cond_broadcast(&ctx->cond_not_full);
        pthread_mutex_unlock(&ctx->lock);

        pthread_join(ctx->worker_thread, NULL);
        ctx->thread_started = 0;
    }

    if (ctx->handle) {
        snd_pcm_close(ctx->handle);
        ctx->handle = NULL;
    }

    pthread_mutex_lock(&ctx->lock);
    while (ctx->q_count > 0) {
        AVPacket *pkt = ctx->queue[ctx->q_head];
        ctx->q_head = (ctx->q_head + 1) % QUEUE_CAPACITY;
        ctx->q_count--;
        av_packet_free(&pkt);
    }
    pthread_mutex_unlock(&ctx->lock);

    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cond_not_empty);
    pthread_cond_destroy(&ctx->cond_not_full);
    return 0;
}

const AVInputFormat ff_zstr_alsa_demuxer = {
    .name           = "zstr_alsa",
    .long_name      = "zff ALSA Audio Capture Device",
    .priv_data_size = sizeof(ALSADeviceContext),
    .read_header    = alsa_read_header,
    .read_packet    = alsa_read_packet,
    .read_close     = alsa_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &zstr_alsa_class,
};
