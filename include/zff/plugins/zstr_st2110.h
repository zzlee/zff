/*=============================================================================
    zstr_st2110.h — SMPTE ST 2110 Broadcast IP Devices (AVInputFormat / AVOutputFormat)
=============================================================================*/
#pragma once

#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard FFmpeg AVInputFormat device for zstr_st2110_demux (ST 2110 Receiver).
 *
 * Usage via standard FFmpeg API:
 *   avformat_open_input(&in_ctx, "udp://0.0.0.0:20000", zff_find_input_format("zstr_st2110_demux"), &options);
 *   av_read_frame(in_ctx, pkt);
 *   avformat_close_input(&in_ctx);
 *
 * Supported AVDictionary options:
 *   - "host": string (Bind IP address)
 *   - "port": int (Bind UDP port)
 */
extern const AVInputFormat ff_zstr_st2110_demuxer;

/**
 * Standard FFmpeg FFOutputFormat device for zstr_st2110_mux (ST 2110 Sender).
 *
 * Usage via standard FFmpeg API:
 *   avformat_alloc_output_context2(&out_ctx, zff_find_output_format("zstr_st2110_mux"), NULL, "udp://127.0.0.1:20000");
 *   avformat_write_header(out_ctx, &options);
 *   av_write_frame(out_ctx, pkt);
 *   av_write_trailer(out_ctx);
 *   avformat_free_context(out_ctx);
 *
 * Supported AVDictionary options:
 *   - "host": string (Destination IP address)
 *   - "port": int (Destination UDP port)
 *   - "pt": int (RTP Payload Type: 96 for video, 97 for audio)
 */
extern const FFOutputFormat ff_zstr_st2110_muxer;

#ifdef __cplusplus
}
#endif
