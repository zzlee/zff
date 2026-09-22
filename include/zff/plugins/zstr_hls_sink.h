/*=============================================================================
    zstr_hls_sink.h — HLS Segmenter Device (AVOutputFormat)
 =============================================================================*/
#pragma once

#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard FFmpeg FFOutputFormat device for zstr_hls_sink.
 *
 * Drives the native hls muxer internally: video must be H264/HEVC with
 * avcC extradata (Annex-B packets are converted per packet), audio AAC,
 * Opus, or PCM S16LE. Segments + playlist land next to `location`.
 *
 * Usage via standard FFmpeg API:
 *   avformat_alloc_output_context2(&oc, zff_find_output_format("zstr_hls_sink"),
 *                                  NULL, "/tmp/hls/out.m3u8");
 *   // add video/audio streams ...
 *   avformat_write_header(oc, &opts);
 *   av_write_frame(oc, pkt);
 *   av_write_trailer(oc);
 *
 * Supported AVDictionary options:
 *   - "location": string (playlist path, required without URL)
 *   - "hls_time": int (segment seconds, default 2)
 *   - "hls_list_size": int (default 5)
 *   - "segment_type": "mpegts" (default) or "fmp4"
 */
extern const FFOutputFormat ff_zstr_hls_sink_muxer;

#ifdef __cplusplus
}
#endif
