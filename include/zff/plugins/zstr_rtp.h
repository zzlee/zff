/*=============================================================================
    zstr_rtp.h — RFC 3550 Generic RTP Payloader & Depayloader Engine
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavcodec/packet.h>
#include <libavutil/rational.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZSTR_RTP_HEADER_LEN 12
#define ZSTR_RTP_DEFAULT_MTU 1400

typedef enum {
    ZSTR_RTP_CODEC_H264,  /**< RFC 6184 H.264 Video */
    ZSTR_RTP_CODEC_H265,  /**< RFC 7798 H.265 / HEVC Video */
    ZSTR_RTP_CODEC_AAC,   /**< RFC 3640 AAC Audio */
    ZSTR_RTP_CODEC_PCM,   /**< RFC 3551 PCM L16 Audio */
    ZSTR_RTP_CODEC_RAW    /**< Generic opaque payload */
} zstr_rtp_codec_t;

/**
 * Standard 12-byte fixed RFC 3550 RTP Header
 */
typedef struct {
    uint8_t  v_p_x_cc;      /**< V(2), P(1), X(1), CC(4) */
    uint8_t  m_pt;          /**< M(1), PT(7) */
    uint16_t seq;           /**< Sequence Number (Network Big-Endian) */
    uint32_t timestamp;     /**< Timestamp (Network Big-Endian) */
    uint32_t ssrc;          /**< SSRC Identifier (Network Big-Endian) */
} __attribute__((packed)) zstr_rtp_header_t;

typedef struct zstr_rtp_payloader zstr_rtp_payloader_t;
typedef struct zstr_rtp_depayloader zstr_rtp_depayloader_t;

/* ---------------------------------------------------------------------------
 * RTP Payloader API
 * --------------------------------------------------------------------------- */
zstr_rtp_payloader_t* zstr_rtp_payloader_create(zstr_rtp_codec_t codec,
                                                uint8_t payload_type,
                                                uint32_t ssrc,
                                                uint32_t clock_rate,
                                                int mtu);

/**
 * Packetize an input access unit (AVPacket) into one or more RTP packets.
 * Handles FU-A / FU fragmentation when packet size exceeds MTU.
 *
 * @param s            Payloader instance.
 * @param in           Input codec AVPacket.
 * @param out_pkts     Pointer to an array of output AVPackets (allocated by caller or internally).
 * @param nb_out_pkts  Output count of RTP packets produced.
 * @return 0 on success, negative AVERROR on failure.
 */
int zstr_rtp_payloader_process(zstr_rtp_payloader_t *s,
                               const AVPacket *in,
                               AVPacket ***out_pkts,
                               int *nb_out_pkts);

void zstr_rtp_payloader_free_packets(AVPacket **pkts, int count);
void zstr_rtp_payloader_free(zstr_rtp_payloader_t **s);

/* ---------------------------------------------------------------------------
 * RTP Depayloader API
 * --------------------------------------------------------------------------- */
zstr_rtp_depayloader_t* zstr_rtp_depayloader_create(zstr_rtp_codec_t codec,
                                                    uint8_t payload_type,
                                                    uint32_t clock_rate);

/**
 * Depacketize / Reassemble incoming RTP packets into a complete codec AVPacket.
 *
 * @param s        Depayloader instance.
 * @param rtp_pkt  Single incoming RTP AVPacket.
 * @param out_pkt  Output AVPacket (allocated by caller; filled when a full AU is assembled).
 * @param ready    Set to true if out_pkt is ready, false if waiting for more fragments.
 * @return 0 on success, negative AVERROR on failure.
 */
int zstr_rtp_depayloader_process(zstr_rtp_depayloader_t *s,
                                 const AVPacket *rtp_pkt,
                                 AVPacket *out_pkt,
                                 bool *ready);

void zstr_rtp_depayloader_free(zstr_rtp_depayloader_t **s);

#ifdef __cplusplus
}
#endif
