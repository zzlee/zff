/*=============================================================================
    zstr_gl_comp.h — OpenGL Video Compositor & Multi-Stream Sink Device (AVOutputFormat)
=============================================================================*/
#pragma once

#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard FFmpeg FFOutputFormat device for zstr_gl_comp.
 * Allows writing multiple video streams (stream 0, 1, ...) via av_write_frame()
 * which are composited and presented in real time.
 *
 * Usage via standard FFmpeg API:
 *   avformat_alloc_output_context2(&out_ctx, zff_find_output_format("zstr_gl_comp"), NULL, NULL);
 *   avformat_write_header(out_ctx, &options);
 *   av_write_frame(out_ctx, pkt);
 *   av_write_trailer(out_ctx);
 *   avformat_free_context(out_ctx);
 *
 * Supported AVDictionary options:
 *   - "display": string (X11 display string, e.g. ":0", ":99")
 *   - "window_title": string (Window title)
 *   - "is_mock": int (1 for mock/headless mode)
 */
extern const FFOutputFormat ff_zstr_gl_comp_muxer;

#ifdef __cplusplus
}
#endif
