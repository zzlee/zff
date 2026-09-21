/*=============================================================================
    zstr_videotestsrc.c — Video Test Source Input Device (AVInputFormat)
=============================================================================*/
#include "zff/plugins/zstr_videotestsrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define QUEUE_CAPACITY 32

typedef enum {
    PATTERN_BARS = 0,
    PATTERN_GRADIENT,
    PATTERN_CHECKERBOARD,
    PATTERN_NOISE,
    PATTERN_BLACK
} VideoPattern;

typedef struct VideoTestSrcContext {
    const AVClass *av_class;

    char *video_size;
    char *framerate;
    char *pixel_format;
    int pattern;
    int realtime;
    int64_t num_frames;

    int width;
    int height;
    AVRational frame_rate;
    int frame_size;
    enum AVPixelFormat pix_fmt;

    /* Non-YUV420P output: render master then sws-convert per frame */
    struct SwsContext *sws;
    uint8_t *tmp_yuv;
    int tmp_size;

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
} VideoTestSrcContext;

#define OFFSET(x) offsetof(VideoTestSrcContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_videotestsrc_options[] = {
    { "video_size", "Frame size (e.g. 1920x1080, hd720)", OFFSET(video_size), AV_OPT_TYPE_STRING, { .str = "1280x720" }, 0, 0, DEC },
    { "framerate",  "Video framerate",                    OFFSET(framerate),  AV_OPT_TYPE_STRING, { .str = "30" },       0, 0, DEC },
    { "pixel_format", "Output pixel format (yuv420p/i420, nv12, nv16, yuyv422, rgb24, bgr24, rgba, bgra)", OFFSET(pixel_format), AV_OPT_TYPE_STRING, { .str = "yuv420p" }, 0, 0, DEC },
    { "pattern",    "Test pattern",                        OFFSET(pattern),    AV_OPT_TYPE_INT,    { .i64 = PATTERN_BARS }, 0, 4, DEC, "pattern" },
        { "bars",         "SMPTE color bars",    0, AV_OPT_TYPE_CONST, { .i64 = PATTERN_BARS },         0, 0, DEC, "pattern" },
        { "gradient",     "Horizontal gradient", 0, AV_OPT_TYPE_CONST, { .i64 = PATTERN_GRADIENT },     0, 0, DEC, "pattern" },
        { "checkerboard", "Checkerboard",        0, AV_OPT_TYPE_CONST, { .i64 = PATTERN_CHECKERBOARD }, 0, 0, DEC, "pattern" },
        { "noise",        "Uniform noise",       0, AV_OPT_TYPE_CONST, { .i64 = PATTERN_NOISE },        0, 0, DEC, "pattern" },
        { "black",        "Solid black",         0, AV_OPT_TYPE_CONST, { .i64 = PATTERN_BLACK },        0, 0, DEC, "pattern" },
    { "realtime",   "Real-time clock pacing (1=on, 0=burst)", OFFSET(realtime), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, DEC },
    { "num_frames", "Max frames to generate (0=infinite)",    OFFSET(num_frames), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT64_MAX, DEC },
    { NULL }
};

static const AVClass zstr_videotestsrc_class = {
    .class_name = "zstr_videotestsrc",
    .item_name  = av_default_item_name,
    .option     = zstr_videotestsrc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const uint8_t smpte_yuv[8][3] = {
    { 235, 128, 128 }, /* White */
    { 210,  16, 146 }, /* Yellow */
    { 170, 166,  16 }, /* Cyan */
    { 145,  54,  34 }, /* Green */
    { 106, 202, 222 }, /* Magenta */
    {  81,  90, 240 }, /* Red */
    {  41, 240, 110 }, /* Blue */
    {  16, 128, 128 }  /* Black */
};

static void render_frame_yuv420p(VideoTestSrcContext *ctx, uint8_t *data) {
    int w = ctx->width;
    int h = ctx->height;
    uint8_t *y = data;
    uint8_t *u = data + (w * h);
    uint8_t *v = u + (w * h / 4);

    switch (ctx->pattern) {
        case PATTERN_BARS: {
            int bar_w = w / 8;
            for (int r = 0; r < h; r++) {
                for (int c = 0; c < w; c++) {
                    int b = c / bar_w;
                    if (b > 7) b = 7;
                    y[r * w + c] = smpte_yuv[b][0];
                    if ((r % 2 == 0) && (c % 2 == 0)) {
                        u[(r / 2) * (w / 2) + (c / 2)] = smpte_yuv[b][1];
                        v[(r / 2) * (w / 2) + (c / 2)] = smpte_yuv[b][2];
                    }
                }
            }
            break;
        }
        case PATTERN_GRADIENT: {
            for (int r = 0; r < h; r++) {
                for (int c = 0; c < w; c++) {
                    y[r * w + c] = (uint8_t)((c * 219) / (w > 1 ? w - 1 : 1) + 16);
                    if ((r % 2 == 0) && (c % 2 == 0)) {
                        u[(r / 2) * (w / 2) + (c / 2)] = 128;
                        v[(r / 2) * (w / 2) + (c / 2)] = 128;
                    }
                }
            }
            break;
        }
        case PATTERN_CHECKERBOARD: {
            const int tile = 32;
            for (int r = 0; r < h; r++) {
                for (int c = 0; c < w; c++) {
                    y[r * w + c] = (((r / tile) ^ (c / tile)) & 1) ? 235 : 16;
                    if ((r % 2 == 0) && (c % 2 == 0)) {
                        u[(r / 2) * (w / 2) + (c / 2)] = 128;
                        v[(r / 2) * (w / 2) + (c / 2)] = 128;
                    }
                }
            }
            break;
        }
        case PATTERN_NOISE: {
            for (int i = 0; i < w * h; i++) y[i] = (uint8_t)(rand() % 256);
            for (int i = 0; i < (w * h) / 4; i++) {
                u[i] = (uint8_t)(rand() % 256);
                v[i] = (uint8_t)(rand() % 256);
            }
            break;
        }
        case PATTERN_BLACK:
        default: {
            memset(y, 16, w * h);
            memset(u, 128, (w * h) / 4);
            memset(v, 128, (w * h) / 4);
            break;
        }
    }
}

static void* videotestsrc_worker(void *arg) {
    VideoTestSrcContext *ctx = (VideoTestSrcContext*)arg;
    int64_t frame_count = 0;

    int64_t interval_ns = (1000000000LL * ctx->frame_rate.den) / ctx->frame_rate.num;
    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

    while (1) {
        pthread_mutex_lock(&ctx->lock);
        if (ctx->stop_requested) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }
        if (ctx->num_frames > 0 && frame_count >= ctx->num_frames) {
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

        /* Real-time pacing */
        if (ctx->realtime) {
            next_time.tv_nsec += interval_ns;
            while (next_time.tv_nsec >= 1000000000L) {
                next_time.tv_sec += 1;
                next_time.tv_nsec -= 1000000000L;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
        }

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) break;
        if (av_new_packet(pkt, ctx->frame_size) < 0) {
            av_packet_free(&pkt);
            break;
        }

        if (ctx->pix_fmt != AV_PIX_FMT_YUV420P && ctx->sws && ctx->tmp_yuv) {
            /* Master is YUV420P in tmp; convert into the packet buffer */
            render_frame_yuv420p(ctx, ctx->tmp_yuv);
            int w = ctx->width, h = ctx->height;
            const uint8_t *src[4] = {
                ctx->tmp_yuv, ctx->tmp_yuv + w * h,
                ctx->tmp_yuv + w * h * 5 / 4, NULL
            };
            int src_ls[4] = { w, w / 2, w / 2, 0 };
            uint8_t *dst[4] = { NULL };
            int dst_ls[4] = { 0 };
            av_image_fill_arrays(dst, dst_ls, pkt->data,
                                 ctx->pix_fmt, w, h, 1);
            sws_scale(ctx->sws, src, src_ls, 0, h, dst, dst_ls);
        } else {
            render_frame_yuv420p(ctx, pkt->data);
        }
        pkt->pts = frame_count;
        pkt->dts = frame_count;
        pkt->stream_index = 0;
        frame_count++;

        pthread_mutex_lock(&ctx->lock);
        ctx->queue[ctx->q_tail] = pkt;
        ctx->q_tail = (ctx->q_tail + 1) % QUEUE_CAPACITY;
        ctx->q_count++;
        pthread_cond_signal(&ctx->cond_not_empty);
        pthread_mutex_unlock(&ctx->lock);
    }
    return NULL;
}

static int videotestsrc_read_header(AVFormatContext *s) {
    VideoTestSrcContext *ctx = s->priv_data;

    if (av_parse_video_size(&ctx->width, &ctx->height, ctx->video_size) < 0) {
        av_log(s, AV_LOG_ERROR, "Invalid video_size: %s\n", ctx->video_size);
        return AVERROR(EINVAL);
    }

    if (av_parse_video_rate(&ctx->frame_rate, ctx->framerate) < 0 ||
        ctx->frame_rate.num <= 0 || ctx->frame_rate.den <= 0) {
        av_log(s, AV_LOG_ERROR, "Invalid framerate: %s\n", ctx->framerate);
        return AVERROR(EINVAL);
    }

    /* Output pixel format: I420 is an alias of YUV420P */
    const char *pf = ctx->pixel_format ? ctx->pixel_format : "yuv420p";
    if (strcmp(pf, "yuv420p") == 0 || strcmp(pf, "i420") == 0 || strcmp(pf, "I420") == 0)
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    else if (strcmp(pf, "nv12") == 0)
        ctx->pix_fmt = AV_PIX_FMT_NV12;
    else if (strcmp(pf, "nv16") == 0)
        ctx->pix_fmt = AV_PIX_FMT_NV16;
    else if (strcmp(pf, "yuyv422") == 0 || strcmp(pf, "yuyv") == 0)
        ctx->pix_fmt = AV_PIX_FMT_YUYV422;
    else if (strcmp(pf, "rgb24") == 0)
        ctx->pix_fmt = AV_PIX_FMT_RGB24;
    else if (strcmp(pf, "bgr24") == 0)
        ctx->pix_fmt = AV_PIX_FMT_BGR24;
    else if (strcmp(pf, "rgba") == 0)
        ctx->pix_fmt = AV_PIX_FMT_RGBA;
    else if (strcmp(pf, "bgra") == 0)
        ctx->pix_fmt = AV_PIX_FMT_BGRA;
    else {
        av_log(s, AV_LOG_WARNING, "Unknown pixel_format '%s', using yuv420p\n", pf);
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    ctx->frame_size = av_image_get_buffer_size(ctx->pix_fmt, ctx->width, ctx->height, 1);
    if (ctx->frame_size < 0) return ctx->frame_size;

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width      = ctx->width;
    st->codecpar->height     = ctx->height;
    st->codecpar->format     = ctx->pix_fmt;
    st->time_base            = av_inv_q(ctx->frame_rate);

    if (ctx->pix_fmt != AV_PIX_FMT_YUV420P) {
        ctx->tmp_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
                                                 ctx->width, ctx->height, 1);
        ctx->tmp_yuv = av_malloc(ctx->tmp_size);
        if (!ctx->tmp_yuv) return AVERROR(ENOMEM);
        ctx->sws = sws_getContext(ctx->width, ctx->height, AV_PIX_FMT_YUV420P,
                                  ctx->width, ctx->height, ctx->pix_fmt,
                                  SWS_BILINEAR, NULL, NULL, NULL);
        if (!ctx->sws) return AVERROR(ENOMEM);
    }

    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond_not_empty, NULL);
    pthread_cond_init(&ctx->cond_not_full, NULL);

    ctx->q_head = 0;
    ctx->q_tail = 0;
    ctx->q_count = 0;
    ctx->stop_requested = 0;
    ctx->eof_reached = 0;

    if (pthread_create(&ctx->worker_thread, NULL, videotestsrc_worker, ctx) != 0) {
        return AVERROR(EIO);
    }
    ctx->thread_started = 1;
    return 0;
}

static int videotestsrc_read_packet(AVFormatContext *s, AVPacket *pkt) {
    VideoTestSrcContext *ctx = s->priv_data;

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

static int videotestsrc_read_close(AVFormatContext *s) {
    VideoTestSrcContext *ctx = s->priv_data;
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

    if (ctx->sws) {
        sws_freeContext(ctx->sws);
        ctx->sws = NULL;
    }
    av_freep(&ctx->tmp_yuv);
    ctx->tmp_size = 0;
    return 0;
}

const AVInputFormat ff_zstr_videotestsrc_demuxer = {
    .name           = "zstr_videotestsrc",
    .long_name      = "zff Video Test Pattern Generator",
    .priv_data_size = sizeof(VideoTestSrcContext),
    .read_header    = videotestsrc_read_header,
    .read_packet    = videotestsrc_read_packet,
    .read_close     = videotestsrc_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &zstr_videotestsrc_class,
};
