/*=============================================================================
    zstr_scale.c — Video Scaler with SIMD 64-byte Alignment & Dynamic Reconfiguration
=============================================================================*/
#include "zff/plugins/zstr_scale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

struct zstr_scale {
    int target_width;
    int target_height;
    enum AVPixelFormat target_format;
    int flags;
    int align;

    /* Current SwsContext and cached parameters */
    struct SwsContext *sws_ctx;
    int cur_in_w;
    int cur_in_h;
    enum AVPixelFormat cur_in_fmt;
    enum AVColorRange cur_in_range;

    int cur_out_w;
    int cur_out_h;
    enum AVPixelFormat cur_out_fmt;
    enum AVColorRange cur_out_range;
};

static int parse_scale_flags(const char *name) {
    if (!name) return SWS_BILINEAR;
    if (strcmp(name, "bicubic") == 0) return SWS_BICUBIC;
    if (strcmp(name, "fast_bilinear") == 0) return SWS_FAST_BILINEAR;
    if (strcmp(name, "neighbor") == 0 || strcmp(name, "point") == 0) return SWS_POINT;
    if (strcmp(name, "lanczos") == 0) return SWS_LANCZOS;
    if (strcmp(name, "spline") == 0) return SWS_SPLINE;
    return SWS_BILINEAR;
}

zstr_scale_t* zstr_scale_alloc(const char *opt_string) {
    zstr_scale_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->target_width = 0;
    s->target_height = 0;
    s->target_format = AV_PIX_FMT_NONE;
    s->flags = SWS_BILINEAR;
    s->align = 64; /* 64-byte alignment for AVX-512 / AVX2 SIMD */

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

                    if (strcmp(key, "w") == 0 || strcmp(key, "width") == 0) {
                        s->target_width = atoi(val);
                    } else if (strcmp(key, "h") == 0 || strcmp(key, "height") == 0) {
                        s->target_height = atoi(val);
                    } else if (strcmp(key, "format") == 0 || strcmp(key, "pix_fmt") == 0) {
                        s->target_format = av_get_pix_fmt(val);
                    } else if (strcmp(key, "flags") == 0) {
                        s->flags = parse_scale_flags(val);
                    } else if (strcmp(key, "align") == 0) {
                        int a = atoi(val);
                        if (a > 0 && (a & (a - 1)) == 0) { /* Power of 2 */
                            s->align = a;
                        }
                    }
                }
                token = strtok(NULL, ":,");
            }
            free(copy);
        }
    }

    return s;
}

int zstr_scale_process(zstr_scale_t *s, const AVFrame *in, AVFrame *out) {
    if (!s || !in || !out) {
        return AVERROR(EINVAL);
    }
    if (in->width <= 0 || in->height <= 0 || in->format == AV_PIX_FMT_NONE) {
        return AVERROR(EINVAL);
    }

    int dst_w = (s->target_width > 0) ? s->target_width : in->width;
    int dst_h = (s->target_height > 0) ? s->target_height : in->height;
    enum AVPixelFormat dst_fmt = (s->target_format != AV_PIX_FMT_NONE) ? s->target_format : (enum AVPixelFormat)in->format;

    /* Zero-copy passthrough if resolution and format are identical */
    if (in->width == dst_w && in->height == dst_h && in->format == dst_fmt) {
        av_frame_unref(out);
        return av_frame_ref(out, in);
    }

    /* Check if SwsContext needs recreation due to dynamic input/output format changes */
    if (!s->sws_ctx ||
        s->cur_in_w != in->width ||
        s->cur_in_h != in->height ||
        s->cur_in_fmt != (enum AVPixelFormat)in->format ||
        s->cur_in_range != in->color_range ||
        s->cur_out_w != dst_w ||
        s->cur_out_h != dst_h ||
        s->cur_out_fmt != dst_fmt) {

        if (s->sws_ctx) {
            sws_freeContext(s->sws_ctx);
            s->sws_ctx = NULL;
        }

        s->sws_ctx = sws_getContext(
            in->width, in->height, in->format,
            dst_w, dst_h, dst_fmt,
            s->flags, NULL, NULL, NULL
        );

        if (!s->sws_ctx) {
            return AVERROR(ENOMEM);
        }

        s->cur_in_w = in->width;
        s->cur_in_h = in->height;
        s->cur_in_fmt = in->format;
        s->cur_in_range = in->color_range;
        s->cur_out_w = dst_w;
        s->cur_out_h = dst_h;
        s->cur_out_fmt = dst_fmt;
        s->cur_out_range = in->color_range;
    }

    /* Prepare output frame */
    av_frame_unref(out);
    out->width = dst_w;
    out->height = dst_h;
    out->format = dst_fmt;

    int ret = av_frame_get_buffer(out, s->align);
    if (ret < 0) {
        return ret;
    }

    /* Perform scaling */
    ret = sws_scale(
        s->sws_ctx,
        (const uint8_t * const *)in->data,
        in->linesize,
        0,
        in->height,
        out->data,
        out->linesize
    );

    if (ret <= 0) {
        av_frame_unref(out);
        return AVERROR(EINVAL);
    }

    /* Propagate frame metadata, timestamps, and side data */
    av_frame_copy_props(out, in);
    out->pts = in->pts;
    out->pkt_dts = in->pkt_dts;
    out->duration = in->duration;

    return 0;
}

void zstr_scale_free(zstr_scale_t **s) {
    if (!s || !*s) return;
    zstr_scale_t *inst = *s;

    if (inst->sws_ctx) {
        sws_freeContext(inst->sws_ctx);
        inst->sws_ctx = NULL;
    }

    free(inst);
    *s = NULL;
}
