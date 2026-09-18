/*=============================================================================
    zstr_srt.h — SRT (Secure Reliable Transport) Streaming Devices (AVInputFormat / AVOutputFormat)
=============================================================================*/
#pragma once

#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard FFmpeg AVInputFormat device for zstr_srt_src (SRT Receiver).
 *
 * Usage via standard FFmpeg API:
 *   avformat_open_input(&in_ctx, "srt://0.0.0.0:9000?mode=listener&latency=120", zff_find_input_format("zstr_srt_src"), &options);
 *   av_read_frame(in_ctx, pkt);
 *   avformat_close_input(&in_ctx);
 *
 * Supported URL parameters & AVDictionary options:
 *   - "mode": "caller", "listener", "rendezvous"
 *   - "latency": int (milliseconds buffer delay)
 *   - "passphrase": string (AES encryption password)
 *   - "pbkeylen": 16, 24, 32
 *   - "streamid": string
 *   - "payload_size": int (default 1316)
 *   - "timeout": int (milliseconds)
 */
extern const AVInputFormat ff_zstr_srt_source_demuxer;

/**
 * Standard FFmpeg FFOutputFormat device for zstr_srt_sink (SRT Sender).
 *
 * Usage via standard FFmpeg API:
 *   avformat_alloc_output_context2(&out_ctx, zff_find_output_format("zstr_srt_sink"), NULL, "srt://127.0.0.1:9000?mode=caller&latency=120");
 *   avformat_write_header(out_ctx, &options);
 *   av_write_frame(out_ctx, pkt);
 *   av_write_trailer(out_ctx);
 *   avformat_free_context(out_ctx);
 */
extern const FFOutputFormat ff_zstr_srt_sink_muxer;

#ifdef __cplusplus
}
#endif
