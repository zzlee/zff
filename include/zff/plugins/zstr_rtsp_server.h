/*=============================================================================
    zstr_rtsp_server.h — Multi-session RTSP Server (AVOutputFormat service)

    Ported from zstreamer `rtsp_server.c` (multi-session RTSP daemon with
    TCP interleaved + UDP unicast transport). Framework code (zst_pad,
    zst_scheduler, caps negotiation, timestamp pacer) is dropped; the
    protocol core is kept:

    - RTSP state machine: OPTIONS / DESCRIBE / SETUP / PLAY / PAUSE / TEARDOWN
    - SDP generation (H264 avcC sprop-parameter-sets, AAC MPEG4-GENERIC config)
    - RTP fan-out via `zstr_rtp_payloader` (H264/H265) + RFC 3640 AU framing (AAC)
    - RTCP Sender Reports, TCP interleaved framing ($ + channel + len)

    Input model: standard FFmpeg muxer. The application adds 1 video stream
    (H264/H265) and optionally 1 audio stream (AAC), then pushes AVPackets
    with av_write_frame(). Packets are fanned out to all PLAYing clients.

    Usage via standard FFmpeg API:
      avformat_alloc_output_context2(&oc, zff_find_output_format("zstr_rtspserver"),
                                     NULL, "rtsp://0.0.0.0:8554/live");
      // add video/audio streams, set codecpar (codec_id + extradata) ...
      avformat_write_header(oc, &opts);   // binds port, spawns listener
      av_write_frame(oc, pkt);            // fan-out to clients
      av_write_trailer(oc);               // stops listener

    Supported AVDictionary options:
      - "port": int (listen port, default 8554)
      - "mount": string (mount point name, default "live")
      - "force_tcp": int (0/1, reject UDP SETUP when 1, default 0)
      - "mtu": int (RTP MTU, default 1400)
 =============================================================================*/
#pragma once

#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZSTR_RTSP_DEFAULT_PORT 8554
#define ZSTR_RTSP_DEFAULT_MTU 1400

/**
 * Standard FFmpeg FFOutputFormat device for zstr_rtspserver.
 */
extern const FFOutputFormat ff_zstr_rtspserver_muxer;

#ifdef __cplusplus
}
#endif
