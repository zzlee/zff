/*=============================================================================
    zstr_x11sink.c — X11 Software Display Sink Device (FFOutputFormat)

    Ported from zstreamer x11_sink.c, minus the framework (pads/caps):
    - The per-pixel XPutPixel conversion loop is replaced by a single
      sws_scale into the XImage buffer (TrueColor 24/32-bit visuals).
      The XPutPixel path is kept only as a fallback for exotic visuals,
      using the same YUV coefficients and mask packing as the original.
    - Window setup (title, WM_DELETE protocol, event mask) follows the
      original; Expose events re-blit the last frame.
    - No display (or is_mock=1) -> null mode: frames are counted, not
      rendered (same contract as zstr_glsink).
=============================================================================*/
#define _GNU_SOURCE

#include "zff/plugins/zstr_x11sink.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

typedef struct X11SinkContext {
    const AVClass *av_class;

    char *display;
    char *window_title;
    int is_mock;

    int width;
    int height;
    enum AVPixelFormat av_pix_fmt;

    int null_mode;
    Display *x_display;
    int x_screen;
    Visual *x_visual;
    Window x_window;
    GC x_gc;
    XImage *x_image;
    Atom wm_delete_window;

    /* Fast path: sws straight into x_image->data (TrueColor RGB32) */
    int use_sws;
    struct SwsContext *sws;
    enum AVPixelFormat sws_in_fmt;

    /* Last converted frame for Expose re-blit */
    uint8_t *last_frame;
    size_t last_frame_size;

    int64_t frames_rendered;
} X11SinkContext;

#define OFFSET(x) offsetof(X11SinkContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_x11sink_options[] = {
    { "display",      "X11 display string (e.g. :0, :99)", OFFSET(display),      AV_OPT_TYPE_STRING, { .str = NULL },          0, 0, ENC },
    { "window_title", "Window title",                     OFFSET(window_title), AV_OPT_TYPE_STRING, { .str = "zff x11sink" }, 0, 0, ENC },
    { "is_mock",      "Force null-mode fake sink",        OFFSET(is_mock),      AV_OPT_TYPE_BOOL,   { .i64 = 0 },              0, 1, ENC },
    { NULL }
};

static const AVClass zstr_x11sink_class = {
    .class_name = "zstr_x11sink",
    .item_name  = av_default_item_name,
    .option     = zstr_x11sink_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* --- Fallback pixel helpers (from zstreamer x11_sink.c) --- */

static inline uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static unsigned long rgb_to_pixel(X11SinkContext *ctx, uint8_t r, uint8_t g, uint8_t b)
{
    unsigned long pixel = 0;
    if (!ctx->x_visual) return 0;
    /* TrueColor/DirectColor: pack per channel mask (original algorithm) */
    unsigned long masks[3] = {
        ctx->x_visual->red_mask, ctx->x_visual->green_mask, ctx->x_visual->blue_mask
    };
    uint8_t vals[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        unsigned long mask = masks[i];
        if (!mask) continue;
        int shift = 0;
        while (!(mask & 1)) { mask >>= 1; shift++; }
        int bits = 0;
        while (mask & 1) { mask >>= 1; bits++; }
        unsigned long v = bits >= 8 ? (unsigned long)vals[i]
                                    : (unsigned long)(vals[i] >> (8 - bits));
        pixel |= (v << shift);
    }
    return pixel;
}

/* Standard TrueColor layout X server advertises on LE hosts:
 * R=0xff0000 G=0x00ff00 B=0x0000ff, XImage LSBFirst.
 * Memory order is then B,G,R,X per pixel == AV_PIX_FMT_RGB32 (LE). */
static bool visual_is_std_rgb32(X11SinkContext *ctx)
{
    if (!ctx->x_visual || !ctx->x_image) return false;
    if (ctx->x_visual->class != TrueColor) return false;
    if (ctx->x_visual->red_mask != 0xff0000) return false;
    if (ctx->x_visual->green_mask != 0x00ff00) return false;
    if (ctx->x_visual->blue_mask != 0x0000ff) return false;
    if (ctx->x_image->byte_order != LSBFirst) return false;
    if (ctx->x_image->bits_per_pixel != 24 && ctx->x_image->bits_per_pixel != 32)
        return false;
    return true;
}

static void destroy_window(X11SinkContext *ctx)
{
    if (!ctx->x_display) return;
    if (ctx->x_image) {
        ctx->x_image->data = NULL; /* buffer owned by last_frame */
        XDestroyImage(ctx->x_image);
        ctx->x_image = NULL;
    }
    if (ctx->x_gc) {
        XFreeGC(ctx->x_display, ctx->x_gc);
        ctx->x_gc = NULL;
    }
    if (ctx->x_window) {
        XDestroyWindow(ctx->x_display, ctx->x_window);
        ctx->x_window = 0;
    }
}

static int init_x11(X11SinkContext *ctx)
{
    const char *disp_name = (ctx->display && ctx->display[0]) ? ctx->display : getenv("DISPLAY");
    if (!disp_name || !disp_name[0]) {
        ctx->null_mode = 1;
        return 0;
    }

    ctx->x_display = XOpenDisplay(disp_name);
    if (!ctx->x_display) {
        ctx->null_mode = 1;
        return 0;
    }

    ctx->x_screen = DefaultScreen(ctx->x_display);
    ctx->x_visual = DefaultVisual(ctx->x_display, ctx->x_screen);
    int depth = DefaultDepth(ctx->x_display, ctx->x_screen);

    ctx->x_window = XCreateSimpleWindow(ctx->x_display,
                                        RootWindow(ctx->x_display, ctx->x_screen),
                                        0, 0, ctx->width, ctx->height, 0,
                                        BlackPixel(ctx->x_display, ctx->x_screen),
                                        BlackPixel(ctx->x_display, ctx->x_screen));
    if (!ctx->x_window) goto fail;

    XStoreName(ctx->x_display, ctx->x_window,
               ctx->window_title ? ctx->window_title : "zff x11sink");
    XSelectInput(ctx->x_display, ctx->x_window, ExposureMask | StructureNotifyMask);
    ctx->wm_delete_window = XInternAtom(ctx->x_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ctx->x_display, ctx->x_window, &ctx->wm_delete_window, 1);
    XMapWindow(ctx->x_display, ctx->x_window);

    ctx->x_gc = XCreateGC(ctx->x_display, ctx->x_window, 0, NULL);
    if (!ctx->x_gc) goto fail;

    ctx->x_image = XCreateImage(ctx->x_display, ctx->x_visual, (unsigned int)depth,
                                ZPixmap, 0, NULL, ctx->width, ctx->height, 32, 0);
    if (!ctx->x_image) goto fail;

    ctx->last_frame_size = (size_t)ctx->x_image->bytes_per_line * (size_t)ctx->height;
    ctx->last_frame = calloc(1, ctx->last_frame_size ? ctx->last_frame_size : 1);
    if (!ctx->last_frame) goto fail;
    ctx->x_image->data = (char *)ctx->last_frame;

    ctx->use_sws = visual_is_std_rgb32(ctx) ? 1 : 0;
    ctx->null_mode = 0;
    XFlush(ctx->x_display);
    return 0;

fail:
    destroy_window(ctx);
    if (ctx->x_display) {
        XCloseDisplay(ctx->x_display);
        ctx->x_display = NULL;
    }
    ctx->null_mode = 1;
    return 0;
}

/* Slow path for exotic visuals: per-pixel convert (zstreamer algorithm). */
static void convert_fallback(X11SinkContext *ctx, const uint8_t *src[4],
                             const int src_ls[4])
{
    int w = ctx->width, h = ctx->height;
    enum AVPixelFormat fmt = ctx->av_pix_fmt;
    int is_rgb24 = (fmt == AV_PIX_FMT_RGB24);
    int is_bgr24 = (fmt == AV_PIX_FMT_BGR24);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0;
            if (is_rgb24) {
                const uint8_t *px = src[0] + (size_t)y * src_ls[0] + (size_t)x * 3;
                r = px[0]; g = px[1]; b = px[2];
            } else if (is_bgr24) {
                const uint8_t *px = src[0] + (size_t)y * src_ls[0] + (size_t)x * 3;
                b = px[0]; g = px[1]; r = px[2];
            } else {
                /* Assume planar YUV 4:2:0-ish; chroma NULL-safe like original */
                int yy = src[0][(size_t)y * src_ls[0] + x];
                int u = 0, v = 0;
                if (src[1] && src[2]) {
                    u = src[1][(size_t)(y / 2) * src_ls[1] + (x / 2)] - 128;
                    v = src[2][(size_t)(y / 2) * src_ls[2] + (x / 2)] - 128;
                }
                r = clamp_u8(yy + ((91881 * v) >> 16));
                g = clamp_u8(yy - ((22554 * u + 46802 * v) >> 16));
                b = clamp_u8(yy + ((116130 * u) >> 16));
            }
            XPutPixel(ctx->x_image, x, y, rgb_to_pixel(ctx, r, g, b));
        }
    }
}

static int x11sink_write_header(AVFormatContext *s)
{
    X11SinkContext *ctx = s->priv_data;

    if (s->nb_streams < 1 || s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        av_log(s, AV_LOG_ERROR, "zstr_x11sink requires at least one video stream\n");
        return AVERROR(EINVAL);
    }

    AVCodecParameters *par = s->streams[0]->codecpar;
    ctx->width = par->width > 0 ? par->width : 640;
    ctx->height = par->height > 0 ? par->height : 480;
    ctx->av_pix_fmt = par->format != AV_PIX_FMT_NONE ? par->format : AV_PIX_FMT_YUV420P;
    ctx->frames_rendered = 0;

    if (ctx->is_mock) {
        ctx->null_mode = 1;
        return 0;
    }

    return init_x11(ctx);
}

static int x11sink_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    X11SinkContext *ctx = s->priv_data;
    if (!pkt || !pkt->data) return 0;

    if (ctx->null_mode || !ctx->x_display) {
        ctx->frames_rendered++;
        return 0;
    }

    /* Pump X11 events; re-blit on Expose */
    while (XPending(ctx->x_display)) {
        XEvent ev;
        XNextEvent(ctx->x_display, &ev);
        if (ev.type == Expose && ctx->last_frame_size) {
            XPutImage(ctx->x_display, ctx->x_window, ctx->x_gc, ctx->x_image,
                      0, 0, 0, 0, ctx->width, ctx->height);
        }
    }

    int w = ctx->width, h = ctx->height;
    const uint8_t *src[4] = { NULL, NULL, NULL, NULL };
    int src_ls[4] = { 0, 0, 0, 0 };
    if (av_image_fill_arrays((uint8_t **)src, src_ls, pkt->data,
                             ctx->av_pix_fmt, w, h, 1) < 0)
        return AVERROR(EINVAL);

    if (ctx->use_sws) {
        if (!ctx->sws || ctx->sws_in_fmt != ctx->av_pix_fmt) {
            if (ctx->sws) sws_freeContext(ctx->sws);
            ctx->sws = sws_getContext(w, h, ctx->av_pix_fmt,
                                      w, h, AV_PIX_FMT_RGB32,
                                      SWS_BILINEAR, NULL, NULL, NULL);
            ctx->sws_in_fmt = ctx->av_pix_fmt;
            if (!ctx->sws) return AVERROR(EINVAL);
        }
        uint8_t *dst[4] = { ctx->last_frame, NULL, NULL, NULL };
        int dst_ls[4] = { ctx->x_image->bytes_per_line, 0, 0, 0 };
        sws_scale(ctx->sws, src, src_ls, 0, h, dst, dst_ls);
    } else {
        convert_fallback(ctx, src, src_ls);
    }

    XPutImage(ctx->x_display, ctx->x_window, ctx->x_gc, ctx->x_image,
              0, 0, 0, 0, (unsigned int)w, (unsigned int)h);
    XFlush(ctx->x_display);
    ctx->frames_rendered++;
    return 0;
}

static int x11sink_write_trailer(AVFormatContext *s)
{
    X11SinkContext *ctx = s->priv_data;

    if (!ctx->null_mode && ctx->x_display) {
        destroy_window(ctx);
        XCloseDisplay(ctx->x_display);
        ctx->x_display = NULL;
        ctx->x_visual = NULL;
    }

    if (ctx->sws) {
        sws_freeContext(ctx->sws);
        ctx->sws = NULL;
    }
    free(ctx->last_frame);
    ctx->last_frame = NULL;
    ctx->last_frame_size = 0;
    return 0;
}

const FFOutputFormat ff_zstr_x11sink_muxer = {
    .p = {
        .name           = "zstr_x11sink",
        .long_name      = "zff X11 Software Display Sink",
        .extensions     = "",
        .audio_codec    = AV_CODEC_ID_NONE,
        .video_codec    = AV_CODEC_ID_RAWVIDEO,
        .subtitle_codec = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
        .priv_class     = &zstr_x11sink_class,
    },
    .priv_data_size = sizeof(X11SinkContext),
    .write_header   = x11sink_write_header,
    .write_packet   = x11sink_write_packet,
    .write_trailer  = x11sink_write_trailer,
};
