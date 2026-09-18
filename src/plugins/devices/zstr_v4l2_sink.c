/*=============================================================================
    zstr_v4l2_sink.c — V4L2 Video Output Sink Device (AVOutputFormat)
=============================================================================*/
#define _GNU_SOURCE
#include "zff/plugins/zstr_v4l2_sink.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

typedef struct V4L2SinkContext {
    const AVClass *av_class;

    char *device;
    char *pixel_format;
    int is_mock;

    int width;
    int height;
    enum AVPixelFormat av_pix_fmt;
    uint32_t v4l2_pix_fmt;
    int frame_size;

    int fd;
    struct {
        void *start;
        size_t length;
    } buffers[4];
    uint32_t nb_buffers;
    int streaming;

    int64_t frames_written;
    int64_t bytes_written;
} V4L2SinkContext;

#define OFFSET(x) offsetof(V4L2SinkContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_v4l2_sink_options[] = {
    { "device",       "V4L2 output device (e.g. /dev/video1)", OFFSET(device),       AV_OPT_TYPE_STRING, { .str = "/dev/video1" }, 0, 0, ENC },
    { "pixel_format", "Pixel format (yuyv422, yuv420p, nv12)", OFFSET(pixel_format), AV_OPT_TYPE_STRING, { .str = "yuyv422" },     0, 0, ENC },
    { "is_mock",      "Force synthetic mock sink",             OFFSET(is_mock),      AV_OPT_TYPE_BOOL,   { .i64 = 0 },             0, 1, ENC },
    { NULL }
};

static const AVClass zstr_v4l2_sink_class = {
    .class_name = "zstr_v4l2_sink",
    .item_name  = av_default_item_name,
    .option     = zstr_v4l2_sink_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int v4l2_sink_write_header(AVFormatContext *s) {
    V4L2SinkContext *ctx = s->priv_data;

    if (s->nb_streams < 1 || s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        av_log(s, AV_LOG_ERROR, "zstr_v4l2_sink requires at least one video stream\n");
        return AVERROR(EINVAL);
    }

    AVCodecParameters *par = s->streams[0]->codecpar;
    ctx->width = par->width > 0 ? par->width : 640;
    ctx->height = par->height > 0 ? par->height : 480;
    ctx->av_pix_fmt = par->format != AV_PIX_FMT_NONE ? par->format : AV_PIX_FMT_YUYV422;

    if (ctx->av_pix_fmt == AV_PIX_FMT_YUV420P) {
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_YUV420;
    } else if (ctx->av_pix_fmt == AV_PIX_FMT_NV12) {
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_NV12;
    } else {
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_YUYV;
    }

    ctx->frame_size = av_image_get_buffer_size(ctx->av_pix_fmt, ctx->width, ctx->height, 1);
    ctx->frames_written = 0;
    ctx->bytes_written = 0;
    ctx->fd = -1;

    if (!ctx->is_mock) {
        const char *dev_path = (s->url && s->url[0] && strcmp(s->url, "dummy") != 0) ? s->url : ctx->device;
        ctx->fd = open(dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (ctx->fd < 0) {
            ctx->is_mock = 1; /* Fallback to mock sink */
        }
    }

    if (!ctx->is_mock && ctx->fd >= 0) {
        struct v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        fmt.fmt.pix.width = ctx->width;
        fmt.fmt.pix.height = ctx->height;
        fmt.fmt.pix.pixelformat = ctx->v4l2_pix_fmt;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
            close(ctx->fd);
            ctx->fd = -1;
            ctx->is_mock = 1;
        }
    }

    if (!ctx->is_mock && ctx->fd >= 0) {
        struct v4l2_requestbuffers req = {0};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
            close(ctx->fd);
            ctx->fd = -1;
            ctx->is_mock = 1;
        } else {
            ctx->nb_buffers = req.count;
            for (uint32_t i = 0; i < ctx->nb_buffers; i++) {
                struct v4l2_buffer buf = {0};
                buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = i;
                if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
                    ctx->is_mock = 1;
                    break;
                }
                ctx->buffers[i].length = buf.length;
                ctx->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                             MAP_SHARED, ctx->fd, buf.m.offset);
                if (ctx->buffers[i].start == MAP_FAILED) {
                    ctx->is_mock = 1;
                    break;
                }
            }
        }
    }

    return 0;
}

static int v4l2_sink_write_packet(AVFormatContext *s, AVPacket *pkt) {
    V4L2SinkContext *ctx = s->priv_data;
    if (!pkt || !pkt->data) return 0;

    if (!ctx->is_mock && ctx->fd >= 0 && ctx->nb_buffers > 0) {
        uint32_t idx = (uint32_t)(ctx->frames_written % ctx->nb_buffers);
        if (ctx->buffers[idx].start) {
            size_t copy_len = pkt->size < (int)ctx->buffers[idx].length ? pkt->size : ctx->buffers[idx].length;
            memcpy(ctx->buffers[idx].start, pkt->data, copy_len);

            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = idx;
            buf.bytesused = copy_len;
            ioctl(ctx->fd, VIDIOC_QBUF, &buf);

            if (!ctx->streaming) {
                enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
                ioctl(ctx->fd, VIDIOC_STREAMON, &type);
                ctx->streaming = 1;
            }
        }
    }

    ctx->frames_written++;
    ctx->bytes_written += pkt->size;
    return 0;
}

static int v4l2_sink_write_trailer(AVFormatContext *s) {
    V4L2SinkContext *ctx = s->priv_data;

    if (ctx->streaming && ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        ctx->streaming = 0;
    }

    for (uint32_t i = 0; i < ctx->nb_buffers; i++) {
        if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
            munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            ctx->buffers[i].start = NULL;
        }
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    return 0;
}

const FFOutputFormat ff_zstr_v4l2_sink_muxer = {
    .p = {
        .name           = "zstr_v4l2_sink",
        .long_name      = "zff V4L2 Video Output Sink Device",
        .video_codec    = AV_CODEC_ID_RAWVIDEO,
        .audio_codec    = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE,
        .priv_class     = &zstr_v4l2_sink_class,
    },
    .priv_data_size = sizeof(V4L2SinkContext),
    .write_header   = v4l2_sink_write_header,
    .write_packet   = v4l2_sink_write_packet,
    .write_trailer  = v4l2_sink_write_trailer,
};
