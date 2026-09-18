/*=============================================================================
    zstr_st2110_toolkit.h — SMPTE ST 2110-20/30 & ST 2022-7 Protocol Processing Toolkit
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>
#include <libavcodec/packet.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * ST 2110-20: RFC 4175 Uncompressed Video Payloader & Depayloader
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2110_20_payloader zstr_st2110_20_payloader_t;
typedef struct zstr_st2110_20_depayloader zstr_st2110_20_depayloader_t;

typedef struct {
    int width;             /**< Video width */
    int height;            /**< Video height */
    enum AVPixelFormat fmt;/**< Pixel format (AV_PIX_FMT_YUV422P, AV_PIX_FMT_UYVY422, AV_PIX_FMT_RGB24) */
    uint8_t payload_type;  /**< RTP Payload Type (default: 96) */
    uint32_t ssrc;         /**< RTP SSRC */
    int max_payload_bytes; /**< Max RTP payload bytes per packet (default: 1376) */
} zstr_st2110_20_config_t;

zstr_st2110_20_payloader_t* zstr_st2110_20_payloader_create(const zstr_st2110_20_config_t *cfg);
int zstr_st2110_20_payloader_process(zstr_st2110_20_payloader_t *s,
                                     const AVFrame *frame,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts);
void zstr_st2110_20_payloader_free_packets(AVPacket **pkts, int count);
void zstr_st2110_20_payloader_free(zstr_st2110_20_payloader_t **s);

zstr_st2110_20_depayloader_t* zstr_st2110_20_depayloader_create(const zstr_st2110_20_config_t *cfg);
int zstr_st2110_20_depayloader_process(zstr_st2110_20_depayloader_t *s,
                                       const AVPacket *rtp_pkt,
                                       AVFrame *out_frame,
                                       bool *ready);
void zstr_st2110_20_depayloader_free(zstr_st2110_20_depayloader_t **s);

/* ---------------------------------------------------------------------------
 * ST 2110-30: AES67 PCM Audio Payloader & Depayloader
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2110_30_payloader zstr_st2110_30_payloader_t;
typedef struct zstr_st2110_30_depayloader zstr_st2110_30_depayloader_t;

typedef struct {
    int channels;          /**< Number of channels (1..8, default 2) */
    int sample_rate;       /**< Sample rate (default 48000) */
    int bit_depth;         /**< 16 or 24 bit PCM */
    uint8_t payload_type;  /**< RTP Payload Type (default: 97) */
    uint32_t ssrc;         /**< RTP SSRC */
    int packet_time_us;    /**< Packet time in microseconds: 1000 (1ms) or 125 (0.125ms) */
} zstr_st2110_30_config_t;

zstr_st2110_30_payloader_t* zstr_st2110_30_payloader_create(const zstr_st2110_30_config_t *cfg);
int zstr_st2110_30_payloader_process(zstr_st2110_30_payloader_t *s,
                                     const AVFrame *frame,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts);
void zstr_st2110_30_payloader_free(zstr_st2110_30_payloader_t **s);

zstr_st2110_30_depayloader_t* zstr_st2110_30_depayloader_create(const zstr_st2110_30_config_t *cfg);
int zstr_st2110_30_depayloader_process(zstr_st2110_30_depayloader_t *s,
                                       const AVPacket *rtp_pkt,
                                       AVFrame *out_frame,
                                       bool *ready);
void zstr_st2110_30_depayloader_free(zstr_st2110_30_depayloader_t **s);

/* ---------------------------------------------------------------------------
 * SMPTE ST 2022-7: Seamless Hitless Secondary Redundancy Demuxer
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2022_7_demux zstr_st2022_7_demux_t;

zstr_st2022_7_demux_t* zstr_st2022_7_demux_create(void);
/**
 * Push an incoming RTP packet from path A (primary=0) or path B (secondary=1).
 * If the packet is a duplicate, it is dropped (*is_duplicate = true).
 * If new, *is_duplicate = false, and caller can forward/process it.
 */
int zstr_st2022_7_demux_process(zstr_st2022_7_demux_t *s,
                                int path_idx,
                                const AVPacket *in_pkt,
                                bool *is_duplicate);
void zstr_st2022_7_demux_free(zstr_st2022_7_demux_t **s);

#ifdef __cplusplus
}
#endif
