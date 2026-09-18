/*=============================================================================
    zstr_net.h — Network UDP/TCP Source and Sink Devices (AVInputFormat / AVOutputFormat)
=============================================================================*/
#pragma once

#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard FFmpeg AVInputFormat device for zstr_net_src (UDP/TCP Receiver).
 *
 * Usage via standard FFmpeg API:
 *   avformat_open_input(&in_ctx, "udp://0.0.0.0:5004", zff_find_input_format("zstr_net_src"), &options);
 *   av_read_frame(in_ctx, pkt);
 *   avformat_close_input(&in_ctx);
 *
 * Supported URL schemes:
 *   - udp://<host>:<port>
 *   - tcp://<host>:<port> (or tcp://<host>:<port>?listen=1 for TCP server)
 *
 * Supported AVDictionary options:
 *   - "protocol": "udp", "tcp_client", "tcp_server"
 *   - "host": string (IP or hostname)
 *   - "port": int
 *   - "buffer_size": int
 *   - "timeout": int (milliseconds)
 */
extern const AVInputFormat ff_zstr_net_source_demuxer;

/**
 * Standard FFmpeg FFOutputFormat device for zstr_net_sink (UDP/TCP Sender).
 *
 * Usage via standard FFmpeg API:
 *   avformat_alloc_output_context2(&out_ctx, zff_find_output_format("zstr_net_sink"), NULL, "udp://127.0.0.1:5004");
 *   avformat_write_header(out_ctx, &options);
 *   av_write_frame(out_ctx, pkt);
 *   av_write_trailer(out_ctx);
 *   avformat_free_context(out_ctx);
 *
 * Supported AVDictionary options:
 *   - "protocol": "udp", "tcp_client", "tcp_server"
 *   - "host": string
 *   - "port": int
 *   - "timeout": int (milliseconds)
 *   - "pkt_size": int
 */
extern const FFOutputFormat ff_zstr_net_sink_muxer;

#ifdef __cplusplus
}
#endif
