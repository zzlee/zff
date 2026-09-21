/*=============================================================================
    zstr_webrtc.h — WebRTC PeerConnection Engine (libdatachannel)

    Ported from zstreamer `webrtc_endpoint.c` + WebRTC phases 1-10.
    Framework deltas:
    - zst_element/pads/bus -> plain C engine; media flows as AVPacket.
    - Signaling stays app-driven (like zstreamer): create_offer() returns
      SDP, the app transports it (websocket/WHIP/file) and feeds back the
      answer + ICE candidates. No signaling transport is bundled.
    - TWCC/GCC congestion control via zstr_webrtc_twcc (needs the patched
      libdatachannel with outgoing track interceptors); bitrate changes
      arrive through a callback for the app's encoder.
    - Chrome interop (multi-track routing, SDP compat, codec selection)
      is kept in the SDP helpers below.

    Typical loopback / call flow:
      a = zstr_webrtc_alloc("stun=stun.l.google.com:19302");
      b = zstr_webrtc_alloc(NULL);
      zstr_webrtc_add_video_track(a, H264, 96, 90000);
      zstr_webrtc_add_video_track(b, H264, 96, 90000);
      offer = zstr_webrtc_create_offer(a);
      zstr_webrtc_set_remote_description(b, offer, "offer");
      answer = zstr_webrtc_create_answer(b);
      zstr_webrtc_set_remote_description(a, answer, "answer");
      ... exchange ICE candidates both ways, wait for connected ...
      zstr_webrtc_send_media(a, video_pkt);
      zstr_webrtc_recv_media(b, out_pkt, timeout_ms);
 =============================================================================*/
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zstr_webrtc zstr_webrtc_t;

typedef enum {
    ZSTR_WEBRTC_CODEC_H264 = 0,
    ZSTR_WEBRTC_CODEC_H265,
    ZSTR_WEBRTC_CODEC_VP8,
    ZSTR_WEBRTC_CODEC_VP9,
    ZSTR_WEBRTC_CODEC_OPUS,
    ZSTR_WEBRTC_CODEC_PCMU,
    ZSTR_WEBRTC_CODEC_PCMA,
    ZSTR_WEBRTC_CODEC_AAC,
} zstr_webrtc_codec_t;

/** Fired when GCC produces a new target bitrate (drive the encoder). */
typedef void (*zstr_webrtc_bitrate_cb)(uint64_t bitrate_bps, void *user_data);
/** Fired for locally gathered ICE candidates (send to the remote peer). */
typedef void (*zstr_webrtc_ice_cb)(const char *candidate, const char *mid, void *user_data);
/** Fired when a data-channel message arrives. */
typedef void (*zstr_webrtc_dc_message_cb)(const char *label, const uint8_t *data,
                                          size_t size, bool is_binary, void *user_data);

/* Lifecycle (config e.g. "stun=host:port:turn=user:pass@host:port:twcc=1:trickle=0") */
zstr_webrtc_t *zstr_webrtc_alloc(const char *opt_string);
void zstr_webrtc_free(zstr_webrtc_t **s);

void zstr_webrtc_set_bitrate_cb(zstr_webrtc_t *s, zstr_webrtc_bitrate_cb cb, void *user_data);
void zstr_webrtc_set_ice_cb(zstr_webrtc_t *s, zstr_webrtc_ice_cb cb, void *user_data);
void zstr_webrtc_set_dc_message_cb(zstr_webrtc_t *s, zstr_webrtc_dc_message_cb cb, void *user_data);

/* Tracks (call before create_offer/create_answer).
 * Returns the track index (>= 0) or -1 on error. */
int zstr_webrtc_add_video_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                                uint8_t payload_type, uint32_t clock_rate);
int zstr_webrtc_add_audio_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                                uint8_t payload_type, uint32_t clock_rate);

/* Signaling (returned SDP strings are owned by the engine).
 * Fully manual negotiation (disableAutoNegotiation): create_offer() and
 * create_answer() trigger generation explicitly; pre-create tracks and
 * data channels before the offer so they are included. */
const char *zstr_webrtc_create_offer(zstr_webrtc_t *s);
const char *zstr_webrtc_create_answer(zstr_webrtc_t *s);
int zstr_webrtc_set_remote_description(zstr_webrtc_t *s, const char *sdp, const char *type);
int zstr_webrtc_add_ice_candidate(zstr_webrtc_t *s, const char *candidate, const char *mid);

/* Connection state: 0=new/connecting, 1=connected, 2=disconnected/failed/closed */
int zstr_webrtc_connected(zstr_webrtc_t *s);
int zstr_webrtc_wait_connected(zstr_webrtc_t *s, int timeout_ms);

/* ICE gathering: 0 when complete (candidates embedded in local SDP). */
int zstr_webrtc_wait_gathering(zstr_webrtc_t *s, int timeout_ms);
/* Non-trickle helper: wait for gathering, then refresh the returned SDP
 * with the committed (candidate-complete) local description. */
const char *zstr_webrtc_complete_gathering(zstr_webrtc_t *s, int timeout_ms);

/* Media (AVPacket payload is sent as one track message; RTP timestamp
 * derives from pkt pts in seconds * clock rate, like zstreamer) */
int zstr_webrtc_send_media(zstr_webrtc_t *s, int track_idx, const AVPacket *pkt);
/* Blocks up to timeout_ms for the next received frame on any track.
 * Returns 0 with *got_frame set, or AVERROR(ETIMEDOUT). */
int zstr_webrtc_recv_media(zstr_webrtc_t *s, AVPacket *pkt, int *track_idx,
                           int timeout_ms, bool *got_frame);

/* Data channels */
int zstr_webrtc_create_data_channel(zstr_webrtc_t *s, const char *label);
int zstr_webrtc_send_data(zstr_webrtc_t *s, const char *label,
                          const uint8_t *data, size_t size);

/* Current GCC estimate in bps (0 when TWCC disabled/unnegotiated) */
uint64_t zstr_webrtc_bitrate(const zstr_webrtc_t *s);

/* FFmpeg device formats (built only with HAS_WEBRTC) */
extern const FFOutputFormat ff_zstr_webrtc_muxer;
extern const AVInputFormat ff_zstr_webrtc_demuxer;

#ifdef __cplusplus
}
#endif
