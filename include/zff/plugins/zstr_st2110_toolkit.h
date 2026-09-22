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
 * ST 2110-40: Ancillary Data Payloader & Depayloader
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2110_40_payloader zstr_st2110_40_payloader_t;
typedef struct zstr_st2110_40_depayloader zstr_st2110_40_depayloader_t;

typedef struct {
    uint8_t payload_type;  /**< RTP Payload Type (default: 100) */
    uint32_t ssrc;         /**< RTP SSRC */
    int mtu;               /**< Max RTP packet bytes incl. header (default: 1400) */
} zstr_st2110_40_config_t;

zstr_st2110_40_payloader_t *zstr_st2110_40_payloader_create(
    const zstr_st2110_40_config_t *cfg);
int zstr_st2110_40_payloader_process(zstr_st2110_40_payloader_t *s,
                                     const AVPacket *in,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts);
void zstr_st2110_40_payloader_free_packets(AVPacket **pkts, int count);
void zstr_st2110_40_payloader_free(zstr_st2110_40_payloader_t **s);

zstr_st2110_40_depayloader_t *zstr_st2110_40_depayloader_create(
    const zstr_st2110_40_config_t *cfg);
int zstr_st2110_40_depayloader_process(zstr_st2110_40_depayloader_t *s,
                                       const AVPacket *rtp_pkt,
                                       AVPacket *out_pkt,
                                       bool *ready);
void zstr_st2110_40_depayloader_free(zstr_st2110_40_depayloader_t **s);

/* ---------------------------------------------------------------------------
 * ST 2110-21: Narrow Sender Traffic Shaper (pacer)
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2110_21_pacer zstr_st2110_21_pacer_t;

typedef struct {
    int width;       /**< default 1920 */
    int height;      /**< default 1080 */
    int fps_num;     /**< default 60000 */
    int fps_den;     /**< default 1001 */
    int pacer_type;  /**< 0 = Narrow linear (only profile in v1) */
} zstr_st2110_21_config_t;

zstr_st2110_21_pacer_t *zstr_st2110_21_pacer_create(
    const zstr_st2110_21_config_t *cfg);
/** Begin a frame of nb_packets with RTP timestamp rtp_ts. */
int zstr_st2110_21_frame_start(zstr_st2110_21_pacer_t *s, uint32_t rtp_ts,
                               int nb_packets);
/** Block until the next packet's Narrow departure slot (0 = send now). */
int zstr_st2110_21_wait_packet(zstr_st2110_21_pacer_t *s);
/** Slots missed so far (diagnostics). */
uint64_t zstr_st2110_21_late_count(const zstr_st2110_21_pacer_t *s);
/** Current per-packet interval in ns (diagnostics). */
int64_t zstr_st2110_21_packet_interval_ns(const zstr_st2110_21_pacer_t *s);
void zstr_st2110_21_pacer_free(zstr_st2110_21_pacer_t **ps);

/* ---------------------------------------------------------------------------
 * ST 2110-22: JPEG XS encode + RFC 9134 codestream packetization + decode
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2110_22_encoder zstr_st2110_22_encoder_t;
typedef struct zstr_st2110_22_payloader zstr_st2110_22_payloader_t;
typedef struct zstr_st2110_22_depayloader zstr_st2110_22_depayloader_t;
typedef struct zstr_st2110_22_decoder zstr_st2110_22_decoder_t;

typedef struct {
    int width;          /**< default 640 */
    int height;         /**< default 480 */
    int fps_num;        /**< default 60 */
    int fps_den;        /**< default 1 */
    int bpp_num;        /**< bits per pixel numerator (default 3) */
    int bpp_den;        /**< bits per pixel denominator (default 1) */
    uint8_t payload_type; /**< RTP PT (default 96) */
    uint32_t ssrc;      /**< RTP SSRC */
    int mtu;            /**< max RTP bytes incl. headers (default 1400) */
} zstr_st2110_22_config_t;

zstr_st2110_22_encoder_t *zstr_st2110_22_encoder_create(
    const zstr_st2110_22_config_t *cfg);
/* AVFrame YUV422P -> JPEG XS codestream AVPacket (90 kHz time base). */
int zstr_st2110_22_encode(zstr_st2110_22_encoder_t *s, const AVFrame *frame,
                          AVPacket **out_pkt);
void zstr_st2110_22_encoder_free(zstr_st2110_22_encoder_t **ps);

zstr_st2110_22_payloader_t *zstr_st2110_22_payloader_create(
    const zstr_st2110_22_config_t *cfg);
int zstr_st2110_22_payloader_process(zstr_st2110_22_payloader_t *s,
                                     const AVPacket *in,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts);
void zstr_st2110_22_payloader_free_packets(AVPacket **pkts, int count);
void zstr_st2110_22_payloader_free(zstr_st2110_22_payloader_t **ps);

zstr_st2110_22_depayloader_t *zstr_st2110_22_depayloader_create(
    const zstr_st2110_22_config_t *cfg);
int zstr_st2110_22_depayloader_process(zstr_st2110_22_depayloader_t *s,
                                       const AVPacket *rtp_pkt,
                                       AVPacket *out_pkt,
                                       bool *ready);
void zstr_st2110_22_depayloader_free(zstr_st2110_22_depayloader_t **ps);

zstr_st2110_22_decoder_t *zstr_st2110_22_decoder_create(
    const zstr_st2110_22_config_t *cfg);
/* JPEG XS codestream AVPacket -> AVFrame YUV422P. */
int zstr_st2110_22_decode(zstr_st2110_22_decoder_t *s, const AVPacket *in,
                          AVFrame **out_frame);
void zstr_st2110_22_decoder_free(zstr_st2110_22_decoder_t **ps);

/* ---------------------------------------------------------------------------
 * ST 2022-5: 1-D Row FEC (XOR protection, v1 row-only)
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2022_5_fec zstr_st2022_5_fec_t;
typedef struct zstr_st2022_5_fec_decoder zstr_st2022_5_fec_decoder_t;

typedef struct {
    int row_len;      /**< media packets per FEC packet, 2..24 (default 4) */
    uint8_t fec_pt;   /**< FEC payload type (default 127; 0 = decoder accepts any) */
} zstr_st2022_5_config_t;

zstr_st2022_5_fec_t *zstr_st2022_5_fec_create(const zstr_st2022_5_config_t *cfg);
/* Returns an FEC AVPacket* when the row completes, else NULL. */
AVPacket *zstr_st2022_5_fec_encode(zstr_st2022_5_fec_t *s, const AVPacket *media);
void zstr_st2022_5_fec_free(zstr_st2022_5_fec_t **ps);

zstr_st2022_5_fec_decoder_t *zstr_st2022_5_fec_decoder_create(
    const zstr_st2022_5_config_t *cfg, uint8_t media_pt);
int zstr_st2022_5_fec_decoder_media(zstr_st2022_5_fec_decoder_t *s,
                                    const AVPacket *media);
/* Returns a recovered AVPacket* when exactly one packet was missing. */
AVPacket *zstr_st2022_5_fec_decoder_fec(zstr_st2022_5_fec_decoder_t *s,
                                       const AVPacket *fec);
uint64_t zstr_st2022_5_fec_recovered(const zstr_st2022_5_fec_decoder_t *s);
uint64_t zstr_st2022_5_fec_unrecoverable(const zstr_st2022_5_fec_decoder_t *s);
void zstr_st2022_5_fec_decoder_free(zstr_st2022_5_fec_decoder_t **ps);

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

/* ---------------------------------------------------------------------------
 * SMPTE ST 2022-7: Seamless Redundancy Sender (dual-send mux)
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2022_7_mux zstr_st2022_7_mux_t;

zstr_st2022_7_mux_t *zstr_st2022_7_mux_create(void);
/**
 * Clone in_pkt into out_a (primary) and out_b (secondary); both owned by
 * the caller. The copies are bit-identical per ST 2022-7.
 */
int zstr_st2022_7_mux_process(zstr_st2022_7_mux_t *s,
                              const AVPacket *in_pkt,
                              AVPacket **out_a,
                              AVPacket **out_b);
uint64_t zstr_st2022_7_mux_count(const zstr_st2022_7_mux_t *s);
void zstr_st2022_7_mux_free(zstr_st2022_7_mux_t **ps);

#ifdef __cplusplus
}
#endif
