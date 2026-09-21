/*=============================================================================
    zstr_webrtc_twcc.h — Transport-Wide Congestion Control (TWCC / GCC)

    Ported from zstreamer `zst_webrtc_twcc.c` (RFC 8888 CCFB + Google
    Congestion Control). Drops zst_bus / zst_log; bitrate changes are
    delivered via a plain C callback so the application can drive its
    encoder (e.g. x264 param_reconfig or FFmpeg encoder bit_rate).
 =============================================================================*/
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zstr_webrtc_twcc zstr_webrtc_twcc_t;

/** Called when the GCC combined estimate changes significantly (> 3%). */
typedef void (*zstr_twcc_bitrate_cb)(uint64_t bitrate_bps, void *user_data);

zstr_webrtc_twcc_t *zstr_webrtc_twcc_create(zstr_twcc_bitrate_cb cb,
                                            void *user_data);
void zstr_webrtc_twcc_destroy(zstr_webrtc_twcc_t *twcc);

/** Feed one incoming RTCP packet (detects TWCC CCFB, fmt=15). Pass-through otherwise. */
void *zstr_webrtc_twcc_process_incoming(zstr_webrtc_twcc_t *twcc,
                                        const char *message, int size);

/** Stamp one outgoing RTP packet with the transport-cc extension.
 * Returns an opaque rtc message (caller passes through to libdatachannel),
 * or the original message pointer when no injection applies. */
void *zstr_webrtc_twcc_process_outgoing(zstr_webrtc_twcc_t *twcc,
                                        const char *message, int size);

/** Find the TWCC extmap ID in a remote offer SDP, or -1. */
int zstr_webrtc_twcc_parse_offer(zstr_webrtc_twcc_t *twcc,
                                 const char *offer_sdp);

/** Inject the TWCC extmap line into a local answer SDP (in place). */
int zstr_webrtc_twcc_inject_answer(zstr_webrtc_twcc_t *twcc,
                                   char *answer_sdp, size_t max_len);

/** Current combined GCC estimate in bps. */
uint64_t zstr_webrtc_twcc_bitrate(const zstr_webrtc_twcc_t *twcc);

#ifdef __cplusplus
}
#endif
