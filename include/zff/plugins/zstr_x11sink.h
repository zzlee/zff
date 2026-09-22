/*=============================================================================
    zstr_x11sink.h — X11 Software Display Sink Device (AVOutputFormat)
 =============================================================================*/
#pragma once

#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard FFmpeg FFOutputFormat device for zstr_x11sink.
 *
 * Software X11 display sink: any input AVPixelFormat is converted with
 * sws_scale straight into the XImage (TrueColor 24/32-bit fast path;
 * per-pixel fallback for exotic visuals) and shown with XPutImage.
 * No display available (or "is_mock"=1) -> null mode: frames counted.
 *
 * Usage via standard FFmpeg API:
 *   avformat_alloc_output_context2(&oc, zff_find_output_format("zstr_x11sink"),
 *                                  NULL, "display");
 *   // add one RAWVIDEO video stream ...
 *   AVDictionary *opts = NULL;
 *   av_dict_set(&opts, "display", ":0", 0);   // or rely on $DISPLAY
 *   avformat_write_header(oc, &opts);
 *   av_write_frame(oc, pkt);                  // pkt->data: packed frame
 *   av_write_trailer(oc);
 *
 * Supported AVDictionary options:
 *   - "display": string (default: $DISPLAY)
 *   - "window_title": string
 *   - "is_mock": bool
 */
extern const FFOutputFormat ff_zstr_x11sink_muxer;

#ifdef __cplusplus
}
#endif
