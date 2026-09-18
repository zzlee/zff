/*=============================================================================
    zstr_videotestsrc.c — Synthetic Video Test Signal Generator for FFmpeg
=============================================================================*/
#include "zff/plugins/zstr_videotestsrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersrc.h>

#define OFFSET(x) offsetof(zstr_videotestsrc_t, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption zstr_videotestsrc_options[] = {
    { "w",          "Video width",             OFFSET(width),       AV_OPT_TYPE_INT,        { .i64 = 1920 }, 16, 8192, FLAGS },
    { "width",      "Video width",             OFFSET(width),       AV_OPT_TYPE_INT,        { .i64 = 1920 }, 16, 8192, FLAGS },
    { "h",          "Video height",            OFFSET(height),      AV_OPT_TYPE_INT,        { .i64 = 1080 }, 16, 8192, FLAGS },
    { "height",     "Video height",            OFFSET(height),      AV_OPT_TYPE_INT,        { .i64 = 1080 }, 16, 8192, FLAGS },
    { "rate",       "Framerate",               OFFSET(frame_rate),  AV_OPT_TYPE_VIDEO_RATE, { .str = "30" }, 0, INT_MAX, FLAGS },
    { "r",          "Framerate",               OFFSET(frame_rate),  AV_OPT_TYPE_VIDEO_RATE, { .str = "30" }, 0, INT_MAX, FLAGS },
    { "pattern",    "Test pattern",            OFFSET(pattern),     AV_OPT_TYPE_INT,        { .i64 = ZSTR_VIDEO_PATTERN_BARS }, 0, 4, FLAGS, "pattern" },
        { "bars",         "SMPTE color bars",    0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_VIDEO_PATTERN_BARS },         0, 0, FLAGS, "pattern" },
        { "gradient",     "Horizontal gradient", 0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_VIDEO_PATTERN_GRADIENT },     0, 0, FLAGS, "pattern" },
        { "checkerboard", "Checkerboard",        0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_VIDEO_PATTERN_CHECKERBOARD }, 0, 0, FLAGS, "pattern" },
        { "noise",        "Uniform noise",       0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_VIDEO_PATTERN_NOISE },        0, 0, FLAGS, "pattern" },
        { "black",        "Solid black",         0, AV_OPT_TYPE_CONST, { .i64 = ZSTR_VIDEO_PATTERN_BLACK },        0, 0, FLAGS, "pattern" },
    { "pix_fmt",    "Pixel format",            OFFSET(pix_fmt),     AV_OPT_TYPE_PIXEL_FMT,  { .i64 = AV_PIX_FMT_YUV420P }, 0, INT_MAX, FLAGS },
    { "num_frames", "Max frames to output",    OFFSET(num_frames),  AV_OPT_TYPE_INT64,      { .i64 = 0 }, 0, INT64_MAX, FLAGS },
    { NULL }
};

static const AVClass zstr_videotestsrc_class = {
    .class_name = "zstr_videotestsrc",
    .item_name  = av_default_item_name,
    .option     = zstr_videotestsrc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* ── 8-Bar SMPTE color values in 8-bit YUV ────────────────────────────── */
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

static void render_bars_yuv420p(zstr_videotestsrc_t *s, uint8_t *y, int y_stride,
                                uint8_t *u, int u_stride, uint8_t *v, int v_stride) {
    int bar_width = s->width / 8;
    for (int r = 0; r < s->height; r++) {
        for (int c = 0; c < s->width; c++) {
            int bar_idx = c / bar_width;
            if (bar_idx > 7) bar_idx = 7;
            y[r * y_stride + c] = smpte_yuv[bar_idx][0];
            if ((r % 2 == 0) && (c % 2 == 0)) {
                int uv_r = r / 2;
                int uv_c = c / 2;
                u[uv_r * u_stride + uv_c] = smpte_yuv[bar_idx][1];
                v[uv_r * v_stride + uv_c] = smpte_yuv[bar_idx][2];
            }
        }
    }
}

static void render_gradient_yuv420p(zstr_videotestsrc_t *s, uint8_t *y, int y_stride,
                                    uint8_t *u, int u_stride, uint8_t *v, int v_stride) {
    for (int r = 0; r < s->height; r++) {
        for (int c = 0; c < s->width; c++) {
            y[r * y_stride + c] = (uint8_t)((c * 235) / (s->width > 1 ? s->width - 1 : 1) + 16);
            if ((r % 2 == 0) && (c % 2 == 0)) {
                int uv_r = r / 2;
                int uv_c = c / 2;
                u[uv_r * u_stride + uv_c] = 128;
                v[uv_r * v_stride + uv_c] = 128;
            }
        }
    }
}

static void render_checkerboard_yuv420p(zstr_videotestsrc_t *s, uint8_t *y, int y_stride,
                                        uint8_t *u, int u_stride, uint8_t *v, int v_stride) {
    const int tile_size = 32;
    for (int r = 0; r < s->height; r++) {
        for (int c = 0; c < s->width; c++) {
            int tile = ((r / tile_size) ^ (c / tile_size)) & 1;
            y[r * y_stride + c] = tile ? 235 : 16;
            if ((r % 2 == 0) && (c % 2 == 0)) {
                int uv_r = r / 2;
                int uv_c = c / 2;
                u[uv_r * u_stride + uv_c] = 128;
                v[uv_r * v_stride + uv_c] = 128;
            }
        }
    }
}

static void render_noise_yuv420p(zstr_videotestsrc_t *s, uint8_t *y, int y_stride,
                                 uint8_t *u, int u_stride, uint8_t *v, int v_stride) {
    for (int r = 0; r < s->height; r++) {
        for (int c = 0; c < s->width; c++) {
            y[r * y_stride + c] = (uint8_t)(rand() % 256);
            if ((r % 2 == 0) && (c % 2 == 0)) {
                int uv_r = r / 2;
                int uv_c = c / 2;
                u[uv_r * u_stride + uv_c] = (uint8_t)(rand() % 256);
                v[uv_r * v_stride + uv_c] = (uint8_t)(rand() % 256);
            }
        }
    }
}

static void render_black_yuv420p(zstr_videotestsrc_t *s, uint8_t *y, int y_stride,
                                 uint8_t *u, int u_stride, uint8_t *v, int v_stride) {
    for (int r = 0; r < s->height; r++) {
        memset(y + r * y_stride, 16, s->width);
    }
    for (int r = 0; r < s->height / 2; r++) {
        memset(u + r * u_stride, 128, s->width / 2);
        memset(v + r * v_stride, 128, s->width / 2);
    }
}

zstr_videotestsrc_t* zstr_videotestsrc_alloc(const char *opt_string) {
    zstr_videotestsrc_t *s = av_mallocz(sizeof(zstr_videotestsrc_t));
    if (!s) return NULL;

    s->av_class = &zstr_videotestsrc_class;
    av_opt_set_defaults(s);

    if (opt_string && *opt_string) {
        if (av_set_options_string(s, opt_string, "=", ":") < 0) {
            zstr_videotestsrc_free(&s);
            return NULL;
        }
    }

    if (s->frame_rate.num <= 0 || s->frame_rate.den <= 0) {
        s->frame_rate = (AVRational){ 30, 1 };
    }

    return s;
}

int zstr_videotestsrc_read_frame(zstr_videotestsrc_t *s, AVFrame *frame) {
    if (!s || !frame) return AVERROR(EINVAL);

    if (s->num_frames > 0 && s->frame_count >= s->num_frames) {
        return AVERROR_EOF;
    }

    frame->width = s->width;
    frame->height = s->height;
    frame->format = s->pix_fmt;
    frame->pts = s->frame_count;

    int ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) return ret;

    ret = av_frame_make_writable(frame);
    if (ret < 0) return ret;

    if (s->pix_fmt == AV_PIX_FMT_YUV420P) {
        uint8_t *y = frame->data[0];
        uint8_t *u = frame->data[1];
        uint8_t *v = frame->data[2];
        int y_stride = frame->linesize[0];
        int u_stride = frame->linesize[1];
        int v_stride = frame->linesize[2];

        switch (s->pattern) {
            case ZSTR_VIDEO_PATTERN_BARS:
                render_bars_yuv420p(s, y, y_stride, u, u_stride, v, v_stride);
                break;
            case ZSTR_VIDEO_PATTERN_GRADIENT:
                render_gradient_yuv420p(s, y, y_stride, u, u_stride, v, v_stride);
                break;
            case ZSTR_VIDEO_PATTERN_CHECKERBOARD:
                render_checkerboard_yuv420p(s, y, y_stride, u, u_stride, v, v_stride);
                break;
            case ZSTR_VIDEO_PATTERN_NOISE:
                render_noise_yuv420p(s, y, y_stride, u, u_stride, v, v_stride);
                break;
            case ZSTR_VIDEO_PATTERN_BLACK:
            default:
                render_black_yuv420p(s, y, y_stride, u, u_stride, v, v_stride);
                break;
        }
    } else {
        /* Generic fallback: render to temp YUV420P buffer, then copy/convert */
        size_t needed = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, s->width, s->height, 1);
        if (!s->tmp_yuv420p || s->tmp_size < needed) {
            av_free(s->tmp_yuv420p);
            s->tmp_yuv420p = av_malloc(needed);
            s->tmp_size = needed;
        }

        uint8_t *ptrs[4];
        int linesizes[4];
        av_image_fill_arrays(ptrs, linesizes, s->tmp_yuv420p, AV_PIX_FMT_YUV420P, s->width, s->height, 1);

        render_bars_yuv420p(s, ptrs[0], linesizes[0], ptrs[1], linesizes[1], ptrs[2], linesizes[2]);
        av_image_copy(frame->data, frame->linesize, (const uint8_t **)ptrs, linesizes, s->pix_fmt, s->width, s->height);
    }

    s->frame_count++;
    return 0;
}

int zstr_videotestsrc_attach_to_graph(zstr_videotestsrc_t *s,
                                      AVFilterGraph *graph,
                                      AVFilterContext **out_src_ctx) {
    if (!s || !graph || !out_src_ctx) return AVERROR(EINVAL);

    const AVFilter *buffer_filter = avfilter_get_by_name("buffer");
    if (!buffer_filter) return AVERROR_FILTER_NOT_FOUND;

    char args[512];
    AVRational time_base = av_inv_q(s->frame_rate);
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             s->width, s->height, s->pix_fmt, time_base.num, time_base.den);

    int ret = avfilter_graph_create_filter(out_src_ctx, buffer_filter,
                                          "zstr_videotestsrc", args, NULL, graph);
    return ret;
}

void zstr_videotestsrc_free(zstr_videotestsrc_t **s) {
    if (s && *s) {
        av_opt_free(*s);
        av_free((*s)->tmp_yuv420p);
        av_freep(s);
    }
}
