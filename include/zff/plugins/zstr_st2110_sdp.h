/*=============================================================================
    zstr_st2110_sdp.h — SMPTE ST 2110 SDP Generate & Parse (RFC 8866 subset)
 =============================================================================*/
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ST 2110 session description. Mirrors the st2110 mode of zstreamer's
 * sdp_muxer (generate) / sdp_demuxer (parse), minus the framework:
 * - Video: raw/90000 (-20) with fmtp sampling/width/height/depth.
 * - Audio: L16/L24 (-30) with mediaclk.
 * - Session: ts-refclk (PTP), keywait.
 */
typedef struct {
    char address[64];       /**< Stream destination (unicast or multicast IP) */
    char session_name[64];
    char ptp_address[64];   /**< PTP grandmaster IP for a=ts-refclk */
    int ptp_domain;         /**< PTP domain number */

    int video_enabled;
    int video_port;
    int video_pt;           /**< RTP payload type (dynamic, e.g. 96) */
    char video_sampling[16];/**< e.g. "YCbCr-4:2:2", "RGB", "YCbCr-4:4:4" */
    int width;
    int height;
    int depth;              /**< bits per component: 8, 10, 12 */

    int audio_enabled;
    int audio_port;
    int audio_pt;
    int sample_rate;        /**< e.g. 48000 */
    int channels;           /**< e.g. 2 */
    int audio_depth;        /**< 16 (L16) or 24 (L24) */
} zstr_st2110_sdp_config_t;

/** Generate an ST 2110 SDP (CRLF line endings). Returns bytes written (<0 on error). */
int zstr_st2110_sdp_generate(const zstr_st2110_sdp_config_t *cfg, char *out, size_t out_size);

/**
 * Parse an ST 2110 SDP into cfg (first video + first audio sections win).
 * Missing optional fields keep their defaults (caller should zero-init or
 * pre-fill; depth defaults to 10, see below). Returns 0 on success.
 */
int zstr_st2110_sdp_parse(const char *sdp, zstr_st2110_sdp_config_t *out_cfg);

#ifdef __cplusplus
}
#endif
