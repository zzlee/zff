/*=============================================================================
    zstr_videotestsrc.h — Synthetic Video Test Signal Generator for FFmpeg
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavfilter/avfilter.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZSTR_VIDEO_PATTERN_BARS,
    ZSTR_VIDEO_PATTERN_GRADIENT,
    ZSTR_VIDEO_PATTERN_CHECKERBOARD,
    ZSTR_VIDEO_PATTERN_NOISE,
    ZSTR_VIDEO_PATTERN_BLACK
} zstr_video_pattern_t;

typedef struct zstr_videotestsrc {
    const AVClass *av_class;

    int width;
    int height;
    AVRational frame_rate;
    int pattern;
    enum AVPixelFormat pix_fmt;

    int64_t num_frames;
    int64_t frame_count;

    /* Internal scratch buffer for color conversions */
    uint8_t *tmp_yuv420p;
    size_t tmp_size;
} zstr_videotestsrc_t;

/**
 * Allocate and initialize a new video test source instance.
 *
 * @param opt_string Optional key=value string (e.g. "w=1920:h=1080:pattern=bars:rate=30").
 * @return Allocated instance, or NULL on error.
 */
zstr_videotestsrc_t* zstr_videotestsrc_alloc(const char *opt_string);

/**
 * Read the next generated video frame.
 *
 * @param s     Pointer to zstr_videotestsrc_t.
 * @param frame Pre-allocated AVFrame to populate.
 * @return 0 on success, AVERROR_EOF if num_frames reached, or negative on error.
 */
int zstr_videotestsrc_read_frame(zstr_videotestsrc_t *s, AVFrame *frame);

/**
 * Convenience helper: Attach this test source to an AVFilterGraph as a buffersrc node.
 *
 * @param s           Valid zstr_videotestsrc_t.
 * @param graph       Valid AVFilterGraph.
 * @param out_src_ctx Output pointer to created buffersrc AVFilterContext.
 * @return 0 on success, negative on error.
 */
int zstr_videotestsrc_attach_to_graph(zstr_videotestsrc_t *s,
                                      AVFilterGraph *graph,
                                      AVFilterContext **out_src_ctx);

/**
 * Free video test source instance.
 */
void zstr_videotestsrc_free(zstr_videotestsrc_t **s);

#ifdef __cplusplus
}
#endif
