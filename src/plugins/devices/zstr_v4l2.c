/*=============================================================================
    zstr_v4l2.c — V4L2 Video Input Device (AVInputFormat)
=============================================================================*/
#define _GNU_SOURCE
#include "zff/plugins/zstr_v4l2.h"
#include "zff/zff_core.h"
#include "zff/zff_time.h"
#include "zff/zff_hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/parseutils.h>

#define MAX_V4L2_BUFFERS 16
#define QUEUE_CAPACITY 32

typedef struct V4L2DeviceContext {
    const AVClass *av_class;

    char *device;
    char *video_size;
    char *framerate;
    char *pixel_format;
    char *memory_type;   /* "mmap", "dmabuf", "mmap-export" */
    char *dmabuf_fds;    /* comma-separated dma-buf fds to import (V4L2_MEMORY_DMABUF) */
    int is_mock;
    int64_t num_frames;
    int realtime;

    int width;
    int height;
    AVRational frame_rate;
    enum AVPixelFormat av_pix_fmt;
    uint32_t v4l2_pix_fmt;
    int frame_size;

    /* Hardware V4L2 state */
    int fd;
    struct {
        void *start;
        size_t length;
    } buffers[MAX_V4L2_BUFFERS];
    int exported_fds[MAX_V4L2_BUFFERS];
    int imported_fds[MAX_V4L2_BUFFERS]; /* fds we queued in DMABUF import mode */
    uint32_t nb_buffers;
    int streaming;

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
} V4L2DeviceContext;

#define OFFSET(x) offsetof(V4L2DeviceContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_v4l2_options[] = {
    { "device",       "V4L2 device node (e.g. /dev/video0)",   OFFSET(device),       AV_OPT_TYPE_STRING, { .str = "/dev/video0" }, 0, 0, DEC },
    { "video_size",   "Capture frame dimensions",              OFFSET(video_size),   AV_OPT_TYPE_STRING, { .str = "640x480" },     0, 0, DEC },
    { "framerate",    "Capture framerate",                     OFFSET(framerate),    AV_OPT_TYPE_STRING, { .str = "30" },          0, 0, DEC },
    { "pixel_format", "Pixel format (yuyv422, yuv420p/i420, nv12, nv16, rgb24, bgr24, rgb32, bgr32)", OFFSET(pixel_format), AV_OPT_TYPE_STRING, { .str = "yuyv422" },     0, 0, DEC },
    { "memory_type",  "Memory mode (mmap, dmabuf, mmap-export)", OFFSET(memory_type), AV_OPT_TYPE_STRING, { .str = "mmap" },        0, 0, DEC },
    { "dmabuf_fds",   "Comma-separated dma-buf fds to import (memory_type=dmabuf)", OFFSET(dmabuf_fds), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, DEC },
    { "is_mock",      "Force synthetic mock fallback",         OFFSET(is_mock),      AV_OPT_TYPE_BOOL,   { .i64 = 0 },             0, 1, DEC },
    { "realtime",     "Real-time clock pacing (1=on, 0=burst)",OFFSET(realtime),     AV_OPT_TYPE_BOOL,   { .i64 = 1 },             0, 1, DEC },
    { "num_frames",   "Max frames to capture (0=infinite)",    OFFSET(num_frames),   AV_OPT_TYPE_INT64,  { .i64 = 0 },             0, INT64_MAX, DEC },
    { NULL }
};

static const AVClass zstr_v4l2_class = {
    .class_name = "zstr_v4l2",
    .item_name  = av_default_item_name,
    .option     = zstr_v4l2_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static void render_mock_pattern(V4L2DeviceContext *ctx, uint8_t *data, int frame_idx) {
    int w = ctx->width;
    int h = ctx->height;

    if (ctx->av_pix_fmt == AV_PIX_FMT_YUYV422) {
        /* YUYV 4:2:2 */
        for (int y = 0; y < h; y++) {
            uint8_t *row = data + y * (w * 2);
            for (int x = 0; x < w; x += 2) {
                uint8_t y_val = ((x + frame_idx * 4) & 0xFF);
                row[x * 2 + 0] = y_val;       // Y0
                row[x * 2 + 1] = 128;         // U
                row[x * 2 + 2] = y_val;       // Y1
                row[x * 2 + 3] = 128;         // V
            }
        }
    } else if (ctx->av_pix_fmt == AV_PIX_FMT_NV12) {
        /* NV12 */
        uint8_t *y_plane = data;
        uint8_t *uv_plane = data + w * h;
        for (int y = 0; y < h; y++) {
            memset(y_plane + y * w, (y + frame_idx * 2) & 0xFF, w);
        }
        memset(uv_plane, 128, w * (h / 2));
    } else if (ctx->av_pix_fmt == AV_PIX_FMT_NV16) {
        /* NV16: like NV12 but full-height interleaved UV */
        uint8_t *y_plane = data;
        uint8_t *uv_plane = data + w * h;
        for (int y = 0; y < h; y++) {
            memset(y_plane + y * w, (y + frame_idx * 2) & 0xFF, w);
        }
        memset(uv_plane, 128, w * h);
    } else if (ctx->av_pix_fmt == AV_PIX_FMT_RGB24 ||
               ctx->av_pix_fmt == AV_PIX_FMT_BGR24) {
        /* Packed 24-bit RGB/BGR: gray ramp */
        for (int y = 0; y < h; y++) {
            uint8_t *row = data + y * (w * 3);
            uint8_t v = (y + frame_idx * 2) & 0xFF;
            for (int x = 0; x < w; x++) {
                row[x * 3 + 0] = v;
                row[x * 3 + 1] = v;
                row[x * 3 + 2] = v;
            }
        }
    } else if (ctx->av_pix_fmt == AV_PIX_FMT_BGR0 ||
               ctx->av_pix_fmt == AV_PIX_FMT_0RGB) {
        /* Packed 32-bit XRGB/XBGR: gray ramp + opaque pad byte */
        for (int y = 0; y < h; y++) {
            uint8_t *row = data + y * (w * 4);
            uint8_t v = (y + frame_idx * 2) & 0xFF;
            for (int x = 0; x < w; x++) {
                row[x * 4 + 0] = v;
                row[x * 4 + 1] = v;
                row[x * 4 + 2] = v;
                row[x * 4 + 3] = 255;
            }
        }
    } else {
        /* YUV420P fallback (I420 is byte-identical) */
        uint8_t *y_plane = data;
        uint8_t *u_plane = data + w * h;
        uint8_t *v_plane = u_plane + (w * h / 4);
        for (int y = 0; y < h; y++) {
            memset(y_plane + y * w, (y + frame_idx * 2) & 0xFF, w);
        }
        memset(u_plane, 128, (w * h) / 4);
        memset(v_plane, 128, (w * h) / 4);
    }
}

static void close_hardware_v4l2(V4L2DeviceContext *ctx) {
    if (ctx->streaming && ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        ctx->streaming = 0;
    }

    for (uint32_t i = 0; i < ctx->nb_buffers; i++) {
        if (ctx->exported_fds[i] >= 0) {
            close(ctx->exported_fds[i]);
            ctx->exported_fds[i] = -1;
        }
        if (ctx->imported_fds[i] >= 0) {
            close(ctx->imported_fds[i]);
            ctx->imported_fds[i] = -1;
        }
        if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
            munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            ctx->buffers[i].start = NULL;
        }
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

static int is_dmabuf_import(V4L2DeviceContext *ctx) {
    return ctx->memory_type && strcmp(ctx->memory_type, "dmabuf") == 0;
}

/* Parse "3,4,5" into fds[]; returns count (0 if none/invalid). */
static int parse_fd_list(const char *s, int *fds, int max) {
    if (!s || !s[0] || max < 1) return 0;
    char *copy = av_strdup(s);
    if (!copy) return 0;
    int n = 0;
    char *tok = strtok(copy, ",");
    while (tok && n < max) {
        long v = strtol(tok, NULL, 10);
        if (v >= 0) fds[n++] = (int)v;
        tok = strtok(NULL, ",");
    }
    av_free(copy);
    return n;
}

/* Fill dmabuf side-data info for the current format/layout. */
static void fill_dmabuf_info(ZffDMABufInfo *info, V4L2DeviceContext *ctx,
                             int fd, uint32_t bytesused) {
    memset(info, 0, sizeof(*info));
    info->fd = fd;
    info->size = (uint32_t)ctx->frame_size;
    info->bytesused = bytesused;
    info->width = ctx->width;
    info->height = ctx->height;

    if (ctx->av_pix_fmt == AV_PIX_FMT_NV12) {
        info->drm_format = ZFF_DRM_FORMAT_NV12;
        info->y_offset   = 0;
        info->y_pitch    = (uint32_t)ctx->width;
        info->uv_offset  = (uint32_t)((size_t)ctx->width * ctx->height);
        info->uv_pitch   = (uint32_t)ctx->width;
    } else if (ctx->av_pix_fmt == AV_PIX_FMT_NV16) {
        info->drm_format = ZFF_DRM_FORMAT_NV16;
        info->y_offset   = 0;
        info->y_pitch    = (uint32_t)ctx->width;
        info->uv_offset  = (uint32_t)((size_t)ctx->width * ctx->height);
        info->uv_pitch   = (uint32_t)ctx->width;
    } else {
        /* Generic packed layout, best effort: 4 bytes per pixel row */
        info->drm_format = 0;
        info->y_offset   = 0;
        info->y_pitch    = (uint32_t)ctx->width * 4;
        info->uv_offset  = 0;
        info->uv_pitch   = 0;
    }
}

static void* v4l2_worker(void *arg) {
    V4L2DeviceContext *ctx = (V4L2DeviceContext *)arg;
    int64_t frame_count = 0;

    int64_t interval_ns = (int64_t)1000000000L * ctx->frame_rate.den / ctx->frame_rate.num;
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

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) break;

        int dmabuf_mode = is_dmabuf_import(ctx);
        if (!dmabuf_mode) {
            /* Payload-carrying modes (mmap / mmap-export): reserve the copy target. */
            if (av_new_packet(pkt, ctx->frame_size) < 0) {
                av_packet_free(&pkt);
                break;
            }
        }

        if (!ctx->is_mock && ctx->fd >= 0) {
            /* Real V4L2 DQBUF */
            struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
            int poll_ret = poll(&pfd, 1, 200);
            if (poll_ret <= 0) {
                av_packet_free(&pkt);
                continue;
            }

            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = dmabuf_mode ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;

            if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
                av_packet_free(&pkt);
                continue;
            }

            if (dmabuf_mode) {
                /* Zero-copy: the driver wrote IN PLACE into our dma-buf fd.
                 * Deliver the fd via FourCC side data, no pixel copy. */
                ZffDMABufInfo info;
                fill_dmabuf_info(&info, ctx, ctx->imported_fds[buf.index], buf.bytesused);
                zff_packet_set_dmabuf(pkt, &info);
                /* Requeue the same imported fd for the next capture. */
                struct v4l2_buffer q = {0};
                q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                q.memory = V4L2_MEMORY_DMABUF;
                q.index = buf.index;
                q.m.fd = ctx->imported_fds[buf.index];
                q.length = (uint32_t)ctx->buffers[buf.index].length;
                ioctl(ctx->fd, VIDIOC_QBUF, &q);
            } else {
                memcpy(pkt->data, ctx->buffers[buf.index].start,
                       buf.bytesused < (uint32_t)ctx->frame_size ? buf.bytesused : (uint32_t)ctx->frame_size);

                /* Requeue buffer */
                ioctl(ctx->fd, VIDIOC_QBUF, &buf);
            }
        } else {
            /* Synthetic mock fallback with real-time pacing */
            if (ctx->realtime) {
                next_time.tv_nsec += interval_ns;
                while (next_time.tv_nsec >= 1000000000L) {
                    next_time.tv_sec += 1;
                    next_time.tv_nsec -= 1000000000L;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
            }

            if (dmabuf_mode) {
                /* Mock DMABUF import: render into the memfd-backed buffer and
                 * hand the fd out via side data (no payload copy). */
                int idx = (int)(frame_count % ctx->nb_buffers);
                if (ctx->buffers[idx].start) {
                    render_mock_pattern(ctx, ctx->buffers[idx].start, (int)frame_count);
                }
                ZffDMABufInfo info;
                fill_dmabuf_info(&info, ctx, ctx->exported_fds[idx], ctx->frame_size);
                zff_packet_set_dmabuf(pkt, &info);
            } else {
                render_mock_pattern(ctx, pkt->data, (int)frame_count);
            }
        }

        /* Set PTS & FourCC PTP Metadata */
        pkt->pts = frame_count;
        pkt->dts = frame_count;
        pkt->stream_index = 0;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        zff_ptp_time_t ptp = {
            .tai_nanoseconds = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec,
            .domain = 0
        };
        zff_packet_set_ptp(pkt, &ptp);

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

static int init_hardware_or_mock_v4l2(V4L2DeviceContext *ctx) {
    for (int i = 0; i < MAX_V4L2_BUFFERS; i++) {
        ctx->exported_fds[i] = -1;
        ctx->imported_fds[i] = -1;
    }

    if (!ctx->is_mock) {
        const char *dev_node = ctx->device ? ctx->device : "/dev/video0";
        ctx->fd = open(dev_node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (ctx->fd < 0) {
            ctx->is_mock = 1; /* Fallback to mock */
        }
    }

    if (!ctx->is_mock && ctx->fd >= 0) {
        struct v4l2_capability cap = {0};
        if (ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0 ||
            !(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
            close(ctx->fd);
            ctx->fd = -1;
            ctx->is_mock = 1;
        }
    }

    if (!ctx->is_mock && ctx->fd >= 0) {
        struct v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
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

    /* ── DMABUF import mode (V4L2_MEMORY_DMABUF) ────────────────────────────
     * The application (or engine) provides dma-buf fds it already owns (e.g.
     * VAAPI surfaces exported via zff/vaExportSurfaceHandle). The driver
     * writes captured frames IN PLACE into those fds — zero pixel copies.
     * The fds ride along with each AVPacket as ZSTR_TAG_DMAB side data. */
    if (!ctx->is_mock && ctx->fd >= 0 && is_dmabuf_import(ctx)) {
        int nfds = parse_fd_list(ctx->dmabuf_fds, ctx->imported_fds, MAX_V4L2_BUFFERS);
        if (nfds < 1) {
            /* No import fds supplied: cannot do real DMABUF import. */
            close(ctx->fd);
            ctx->fd = -1;
            ctx->is_mock = 1;
        } else {
            struct v4l2_requestbuffers req = {0};
            req.count = nfds;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_DMABUF;

            if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < (uint32_t)nfds) {
                close_hardware_v4l2(ctx);
                ctx->is_mock = 1;
            } else {
                ctx->nb_buffers = nfds;
                int ok = 1;
                for (uint32_t i = 0; i < ctx->nb_buffers && ok; i++) {
                    struct v4l2_buffer buf = {0};
                    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    buf.memory = V4L2_MEMORY_DMABUF;
                    buf.index = i;

                    if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
                        ok = 0;
                        break;
                    }
                    ctx->buffers[i].length = buf.length;

                    buf.m.fd = ctx->imported_fds[i];
                    buf.length = ctx->buffers[i].length;
                    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
                        ok = 0;
                        break;
                    }
                }
                if (!ok) {
                    close_hardware_v4l2(ctx);
                    ctx->is_mock = 1;
                } else {
                    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) == 0) {
                        ctx->streaming = 1;
                    } else {
                        close_hardware_v4l2(ctx);
                        ctx->is_mock = 1;
                    }
                }
            }
        }
    }

    /* ── MMAP / MMAP-EXPORT modes (skipped for DMABUF import) ────────────── */
    if (!ctx->is_mock && ctx->fd >= 0 && !is_dmabuf_import(ctx)) {
        struct v4l2_requestbuffers req = {0};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
            close(ctx->fd);
            ctx->fd = -1;
            ctx->is_mock = 1;
        } else {
            ctx->nb_buffers = req.count;
            for (uint32_t i = 0; i < ctx->nb_buffers; i++) {
                struct v4l2_buffer buf = {0};
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = i;

                if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
                    close_hardware_v4l2(ctx);
                    ctx->is_mock = 1;
                    break;
                }

                ctx->buffers[i].length = buf.length;
                ctx->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, ctx->fd, buf.m.offset);
                if (ctx->buffers[i].start == MAP_FAILED) {
                    close_hardware_v4l2(ctx);
                    ctx->is_mock = 1;
                    break;
                }

                /* VIDIOC_EXPBUF DMABUF Export (mmap-export mode only) */
                if (strcmp(ctx->memory_type, "mmap-export") == 0) {
                    struct v4l2_exportbuffer expbuf = {0};
                    expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    expbuf.index = i;
                    expbuf.flags = O_RDWR | O_CLOEXEC;
                    if (ioctl(ctx->fd, VIDIOC_EXPBUF, &expbuf) == 0) {
                        ctx->exported_fds[i] = expbuf.fd;
                    }
                }

                ioctl(ctx->fd, VIDIOC_QBUF, &buf);
            }

            if (!ctx->is_mock) {
                enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) == 0) {
                    ctx->streaming = 1;
                } else {
                    close_hardware_v4l2(ctx);
                    ctx->is_mock = 1;
                }
            }
        }
    }

    /* Mock Mode setup */
    if (ctx->is_mock) {
        ctx->nb_buffers = 4;
        int is_dmabuf = (strcmp(ctx->memory_type, "mmap-export") == 0 ||
                         strcmp(ctx->memory_type, "dmabuf") == 0);

        for (uint32_t i = 0; i < ctx->nb_buffers; i++) {
            ctx->buffers[i].length = ctx->frame_size;
            if (is_dmabuf) {
                /* Simulate DMABUF export using memfd_create */
                int mfd = memfd_create("zstr_v4l2_mock_dmabuf", MFD_CLOEXEC);
                if (mfd >= 0) {
                    if (ftruncate(mfd, ctx->frame_size) == 0) {
                        ctx->buffers[i].start = mmap(NULL, ctx->frame_size,
                                                    PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
                        ctx->exported_fds[i] = mfd;
                    } else {
                        close(mfd);
                    }
                }
            }
            if (!ctx->buffers[i].start || ctx->buffers[i].start == MAP_FAILED) {
                ctx->buffers[i].start = calloc(1, ctx->frame_size);
            }
        }
    }

    return 0;
}

static int v4l2_read_header(AVFormatContext *s) {
    V4L2DeviceContext *ctx = s->priv_data;

    if (av_parse_video_size(&ctx->width, &ctx->height, ctx->video_size) < 0) {
        av_log(s, AV_LOG_ERROR, "Invalid video_size: %s\n", ctx->video_size);
        return AVERROR(EINVAL);
    }

    if (av_parse_video_rate(&ctx->frame_rate, ctx->framerate) < 0 ||
        ctx->frame_rate.num <= 0 || ctx->frame_rate.den <= 0) {
        av_log(s, AV_LOG_ERROR, "Invalid framerate: %s\n", ctx->framerate);
        return AVERROR(EINVAL);
    }

    if (strcmp(ctx->pixel_format, "yuv420p") == 0 || strcmp(ctx->pixel_format, "I420") == 0) {
        ctx->av_pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_YUV420;
    } else if (strcmp(ctx->pixel_format, "nv12") == 0) {
        ctx->av_pix_fmt = AV_PIX_FMT_NV12;
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_NV12;
    } else if (strcmp(ctx->pixel_format, "nv16") == 0) {
        ctx->av_pix_fmt = AV_PIX_FMT_NV16;
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_NV16;
    } else if (strcmp(ctx->pixel_format, "rgb24") == 0) {
        ctx->av_pix_fmt = AV_PIX_FMT_RGB24;
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_RGB24;
    } else if (strcmp(ctx->pixel_format, "bgr24") == 0) {
        ctx->av_pix_fmt = AV_PIX_FMT_BGR24;
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_BGR24;
    } else if (strcmp(ctx->pixel_format, "rgb32") == 0) {
        ctx->av_pix_fmt = AV_PIX_FMT_0RGB;
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_RGB32;
    } else if (strcmp(ctx->pixel_format, "bgr32") == 0) {
        ctx->av_pix_fmt = AV_PIX_FMT_BGR0;
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_BGR32;
    } else {
        ctx->av_pix_fmt = AV_PIX_FMT_YUYV422;
        ctx->v4l2_pix_fmt = V4L2_PIX_FMT_YUYV;
    }

    ctx->frame_size = av_image_get_buffer_size(ctx->av_pix_fmt, ctx->width, ctx->height, 1);
    if (ctx->frame_size < 0) return ctx->frame_size;

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width      = ctx->width;
    st->codecpar->height     = ctx->height;
    st->codecpar->format     = ctx->av_pix_fmt;
    st->time_base            = av_inv_q(ctx->frame_rate);

    init_hardware_or_mock_v4l2(ctx);

    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond_not_empty, NULL);
    pthread_cond_init(&ctx->cond_not_full, NULL);

    ctx->q_head = 0;
    ctx->q_tail = 0;
    ctx->q_count = 0;
    ctx->stop_requested = 0;
    ctx->eof_reached = 0;

    if (pthread_create(&ctx->worker_thread, NULL, v4l2_worker, ctx) != 0) {
        close_hardware_v4l2(ctx);
        return AVERROR(EIO);
    }
    ctx->thread_started = 1;
    return 0;
}

static int v4l2_read_packet(AVFormatContext *s, AVPacket *pkt) {
    V4L2DeviceContext *ctx = s->priv_data;

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

static int v4l2_read_close(AVFormatContext *s) {
    V4L2DeviceContext *ctx = s->priv_data;
    if (ctx->thread_started) {
        pthread_mutex_lock(&ctx->lock);
        ctx->stop_requested = 1;
        pthread_cond_broadcast(&ctx->cond_not_empty);
        pthread_cond_broadcast(&ctx->cond_not_full);
        pthread_mutex_unlock(&ctx->lock);

        pthread_join(ctx->worker_thread, NULL);
        ctx->thread_started = 0;
    }

    close_hardware_v4l2(ctx);

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

const AVInputFormat ff_zstr_v4l2_demuxer = {
    .name           = "zstr_v4l2",
    .long_name      = "zff V4L2 Video Capture Device",
    .priv_data_size = sizeof(V4L2DeviceContext),
    .read_header    = v4l2_read_header,
    .read_packet    = v4l2_read_packet,
    .read_close     = v4l2_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &zstr_v4l2_class,
};
