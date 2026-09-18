/*=============================================================================
    zstr_audiotestsrc.c — Audio Test Source Input Device (AVInputFormat)
=============================================================================*/
#include "zff/plugins/zstr_audiotestsrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>

#define QUEUE_CAPACITY 32

typedef enum {
    WAVE_SINE = 0,
    WAVE_SQUARE,
    WAVE_WHITE_NOISE,
    WAVE_PINK_NOISE,
    WAVE_SILENCE
} AudioWave;

typedef struct AudioTestSrcContext {
    const AVClass *av_class;

    int sample_rate;
    int channels;
    int wave;
    double frequency;
    double volume;
    int samples_per_frame;
    int realtime;
    int64_t num_samples;

    /* Background worker thread & packet queue */
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

    double phase;
    double pink_b0, pink_b1, pink_b2, pink_b3, pink_b4, pink_b5, pink_b6;
} AudioTestSrcContext;

#define OFFSET(x) offsetof(AudioTestSrcContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_audiotestsrc_options[] = {
    { "sample_rate",       "Sample rate in Hz",          OFFSET(sample_rate),       AV_OPT_TYPE_INT,    { .i64 = 48000 }, 8000, 192000, DEC },
    { "channels",          "Number of audio channels",   OFFSET(channels),          AV_OPT_TYPE_INT,    { .i64 = 2 },     1, 8, DEC },
    { "wave",              "Waveform type",              OFFSET(wave),              AV_OPT_TYPE_INT,    { .i64 = WAVE_SINE }, 0, 4, DEC, "wave" },
        { "sine",          "Sine wave",         0, AV_OPT_TYPE_CONST, { .i64 = WAVE_SINE },        0, 0, DEC, "wave" },
        { "square",        "Square wave",       0, AV_OPT_TYPE_CONST, { .i64 = WAVE_SQUARE },      0, 0, DEC, "wave" },
        { "white_noise",   "White noise",       0, AV_OPT_TYPE_CONST, { .i64 = WAVE_WHITE_NOISE }, 0, 0, DEC, "wave" },
        { "pink_noise",    "Pink noise",        0, AV_OPT_TYPE_CONST, { .i64 = WAVE_PINK_NOISE },  0, 0, DEC, "wave" },
        { "silence",       "Silence",           0, AV_OPT_TYPE_CONST, { .i64 = WAVE_SILENCE },     0, 0, DEC, "wave" },
    { "freq",              "Tone frequency (Hz)",        OFFSET(frequency),         AV_OPT_TYPE_DOUBLE, { .dbl = 1000.0 }, 20.0, 20000.0, DEC },
    { "volume",            "Volume level (0.0-1.0)",     OFFSET(volume),            AV_OPT_TYPE_DOUBLE, { .dbl = 0.5 },    0.0, 1.0, DEC },
    { "samples_per_frame", "Samples per buffer packet",  OFFSET(samples_per_frame), AV_OPT_TYPE_INT,    { .i64 = 1024 },  64, 8192, DEC },
    { "realtime",          "Real-time audio pacing",     OFFSET(realtime),          AV_OPT_TYPE_BOOL,   { .i64 = 1 },     0, 1, DEC },
    { "num_samples",       "Max samples (0=infinite)",   OFFSET(num_samples),       AV_OPT_TYPE_INT64,  { .i64 = 0 },     0, INT64_MAX, DEC },
    { NULL }
};

static const AVClass zstr_audiotestsrc_class = {
    .class_name = "zstr_audiotestsrc",
    .item_name  = av_default_item_name,
    .option     = zstr_audiotestsrc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static double generate_sample(AudioTestSrcContext *ctx) {
    double val = 0.0;
    switch (ctx->wave) {
        case WAVE_SINE:
            val = sin(2.0 * M_PI * ctx->phase);
            ctx->phase += ctx->frequency / (double)ctx->sample_rate;
            if (ctx->phase >= 1.0) ctx->phase -= 1.0;
            break;
        case WAVE_SQUARE:
            val = (ctx->phase < 0.5) ? 1.0 : -1.0;
            ctx->phase += ctx->frequency / (double)ctx->sample_rate;
            if (ctx->phase >= 1.0) ctx->phase -= 1.0;
            break;
        case WAVE_WHITE_NOISE:
            val = ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;
            break;
        case WAVE_PINK_NOISE: {
            double white = ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;
            ctx->pink_b0 = 0.99886 * ctx->pink_b0 + white * 0.0555179;
            ctx->pink_b1 = 0.99332 * ctx->pink_b1 + white * 0.0750759;
            ctx->pink_b2 = 0.96900 * ctx->pink_b2 + white * 0.1538520;
            ctx->pink_b3 = 0.86650 * ctx->pink_b3 + white * 0.3104856;
            ctx->pink_b4 = 0.55000 * ctx->pink_b4 + white * 0.5329522;
            ctx->pink_b5 = -0.7616 * ctx->pink_b5 - white * 0.0168980;
            val = (ctx->pink_b0 + ctx->pink_b1 + ctx->pink_b2 + ctx->pink_b3 +
                   ctx->pink_b4 + ctx->pink_b5 + ctx->pink_b6 + white * 0.5362) * 0.11;
            ctx->pink_b6 = white * 0.115926;
            break;
        }
        case WAVE_SILENCE:
        default:
            val = 0.0;
            break;
    }
    return val * ctx->volume;
}

static void* audiotestsrc_worker(void *arg) {
    AudioTestSrcContext *ctx = (AudioTestSrcContext*)arg;
    int64_t total_sent = 0;

    int64_t interval_ns = (1000000000LL * ctx->samples_per_frame) / ctx->sample_rate;
    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

    while (1) {
        pthread_mutex_lock(&ctx->lock);
        if (ctx->stop_requested) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }
        if (ctx->num_samples > 0 && total_sent >= ctx->num_samples) {
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

        /* Real-time audio pacing */
        if (ctx->realtime) {
            next_time.tv_nsec += interval_ns;
            while (next_time.tv_nsec >= 1000000000L) {
                next_time.tv_sec += 1;
                next_time.tv_nsec -= 1000000000L;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
        }

        int nb_samples = ctx->samples_per_frame;
        if (ctx->num_samples > 0 && total_sent + nb_samples > ctx->num_samples) {
            nb_samples = (int)(ctx->num_samples - total_sent);
        }

        int bytes_per_sample = 2 * ctx->channels; // S16LE interleaved
        int pkt_size = nb_samples * bytes_per_sample;

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) break;
        if (av_new_packet(pkt, pkt_size) < 0) {
            av_packet_free(&pkt);
            break;
        }

        int16_t *pcm = (int16_t*)pkt->data;
        for (int i = 0; i < nb_samples; i++) {
            double sample = generate_sample(ctx);
            int16_t v = (int16_t)round(sample * 32767.0);
            for (int ch = 0; ch < ctx->channels; ch++) {
                pcm[i * ctx->channels + ch] = v;
            }
        }

        pkt->pts = total_sent;
        pkt->dts = total_sent;
        pkt->duration = nb_samples;
        pkt->stream_index = 0;
        total_sent += nb_samples;

        pthread_mutex_lock(&ctx->lock);
        ctx->queue[ctx->q_tail] = pkt;
        ctx->q_tail = (ctx->q_tail + 1) % QUEUE_CAPACITY;
        ctx->q_count++;
        pthread_cond_signal(&ctx->cond_not_empty);
        pthread_mutex_unlock(&ctx->lock);
    }
    return NULL;
}

static int audiotestsrc_read_header(AVFormatContext *s) {
    AudioTestSrcContext *ctx = s->priv_data;

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = AV_CODEC_ID_PCM_S16LE;
    st->codecpar->sample_rate = ctx->sample_rate;
    av_channel_layout_default(&st->codecpar->ch_layout, ctx->channels);
    st->time_base            = (AVRational){ 1, ctx->sample_rate };

    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond_not_empty, NULL);
    pthread_cond_init(&ctx->cond_not_full, NULL);

    ctx->q_head = 0;
    ctx->q_tail = 0;
    ctx->q_count = 0;
    ctx->stop_requested = 0;
    ctx->eof_reached = 0;

    if (pthread_create(&ctx->worker_thread, NULL, audiotestsrc_worker, ctx) != 0) {
        return AVERROR(EIO);
    }
    ctx->thread_started = 1;
    return 0;
}

static int audiotestsrc_read_packet(AVFormatContext *s, AVPacket *pkt) {
    AudioTestSrcContext *ctx = s->priv_data;

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

static int audiotestsrc_read_close(AVFormatContext *s) {
    AudioTestSrcContext *ctx = s->priv_data;
    if (ctx->thread_started) {
        pthread_mutex_lock(&ctx->lock);
        ctx->stop_requested = 1;
        pthread_cond_broadcast(&ctx->cond_not_empty);
        pthread_cond_broadcast(&ctx->cond_not_full);
        pthread_mutex_unlock(&ctx->lock);

        pthread_join(ctx->worker_thread, NULL);
        ctx->thread_started = 0;
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

const AVInputFormat ff_zstr_audiotestsrc_demuxer = {
    .name           = "zstr_audiotestsrc",
    .long_name      = "zff Audio Test Signal Generator",
    .priv_data_size = sizeof(AudioTestSrcContext),
    .read_header    = audiotestsrc_read_header,
    .read_packet    = audiotestsrc_read_packet,
    .read_close     = audiotestsrc_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &zstr_audiotestsrc_class,
};
