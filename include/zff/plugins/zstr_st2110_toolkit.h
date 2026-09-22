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
    int pacer_type;  /**< 0 = Narrow linear, 1 = Wide burst-tolerant */
    int cmax;        /**< Wide: max consecutive immediate sends (default 16) */
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
 * ST 2110-21: Receiver compliance monitor (VRX / CMAX / lateness)
 *
 * Thresholds are zff measurement defaults, NOT SMPTE certification values.
 * --------------------------------------------------------------------------- */
typedef struct zstr_st2110_21_monitor zstr_st2110_21_monitor_t;

typedef struct {
    int packets_per_frame; /**< default 100 */
    int fps_num;           /**< default 60000 */
    int fps_den;           /**< default 1001 */
    int vrx_full;          /**< VRX overflow level in packets (default 16) */
    int cmax_limit;        /**< max tolerated burst run (default 8) */
    int64_t late_limit_ns; /**< max tolerated lateness (default 2 intervals) */
} zstr_st2110_21_monitor_config_t;

typedef struct {
    double max_vrx_occ;
    uint64_t overflows;
    int max_burst;
    int64_t max_late_ns;
    uint64_t late_count;
    uint64_t total;
    bool compliant;
} zstr_st2110_21_report_t;

zstr_st2110_21_monitor_t *zstr_st2110_21_monitor_create(
    const zstr_st2110_21_monitor_config_t *cfg);
/** Feed one arrival (16-bit RTP seq, CLOCK_MONOTONIC ns). */
int zstr_st2110_21_monitor_packet(zstr_st2110_21_monitor_t *s, uint16_t seq,
                                  int64_t arrival_ns);
/** Snapshot measurements + threshold verdict. */
int zstr_st2110_21_monitor_check(const zstr_st2110_21_monitor_t *s,
                                 zstr_st2110_21_report_t *report);
void zstr_st2110_21_monitor_free(zstr_st2110_21_monitor_t **ps);

/* ---------------------------------------------------------------------------
 * ST 2110-22: JPEG XS encode + RFC 9134 packetization + decode
 *
 * Two packetization modes (RFC 9134 K bit):
 * - Codestream (K=0): encode() -> payloader_process() -> depayloader_process().
 * - Slice (K=1): encode_units() -> payloader_process_unit() per unit ->
 *   depayloader_process_unit() per unit. SEP is the slice index (0x7FF for
 *   the header unit), L ends each unit, M ends the frame.
 * Slice decode to AVFrame is a follow-up (SVT's decoder cannot consume
 * slice framing piecewise today); transport is complete and byte-exact.
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
    int slice_mode;     /**< RFC 9134 K bit: 0 = codestream, 1 = slice */
    int slice_height;   /**< slice height in luma lines (default 16) */
} zstr_st2110_22_config_t;

zstr_st2110_22_encoder_t *zstr_st2110_22_encoder_create(
    const zstr_st2110_22_config_t *cfg);
/* AVFrame YUV422P -> JPEG XS codestream AVPacket (90 kHz time base).
 * Codestream mode only (EINVAL in slice mode: use encode_units). */
int zstr_st2110_22_encode(zstr_st2110_22_encoder_t *s, const AVFrame *frame,
                          AVPacket **out_pkt);
/* Slice mode: one AVPacket per packetization unit (units[0] = header
 * segment, units[1..] = slices in order). Requires slice_mode. */
int zstr_st2110_22_encode_units(zstr_st2110_22_encoder_t *s, const AVFrame *frame,
                                AVPacket ***out_units, int *nb_units);
void zstr_st2110_22_encode_free_units(AVPacket **units, int count);
void zstr_st2110_22_encoder_free(zstr_st2110_22_encoder_t **ps);

zstr_st2110_22_payloader_t *zstr_st2110_22_payloader_create(
    const zstr_st2110_22_config_t *cfg);
int zstr_st2110_22_payloader_process(zstr_st2110_22_payloader_t *s,
                                     const AVPacket *in,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts);
/* Slice mode (K=1): packetize one unit. sep is 0x7FF for the header unit,
 * else the 0-based slice index. is_last_unit sets M on the final packet.
 * The RFC-required trailing EOC is appended to the last unit when the
 * encoder omitted it (documented SVT gap). F counter advances per frame. */
int zstr_st2110_22_payloader_process_unit(zstr_st2110_22_payloader_t *s,
                                          const AVPacket *unit, int sep,
                                          bool is_last_unit,
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
/* Slice mode (K=1): reassemble one unit (ready=true on its L packet).
 * frame_end is set when the completing packet also carries M.
 * Rejects K=0 packets (use the codestream path for those). */
int zstr_st2110_22_depayloader_process_unit(zstr_st2110_22_depayloader_t *s,
                                            const AVPacket *rtp_pkt,
                                            AVPacket *out_unit,
                                            bool *ready,
                                            bool *frame_end);
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
    int col_len;      /**< rows per matrix, 0/1 = row-only; needs (D-1)*L <= 23 */
} zstr_st2022_5_config_t;

zstr_st2022_5_fec_t *zstr_st2022_5_fec_create(const zstr_st2022_5_config_t *cfg);
/* Returns an FEC AVPacket* when the row completes, else NULL.
 * Row-only API: with col_len > 0 use fec_encode_matrix for column cover. */
AVPacket *zstr_st2022_5_fec_encode(zstr_st2022_5_fec_t *s, const AVPacket *media);
/* Matrix API: collects every FEC packet emitted by this call (row and/or
 * column) into a malloc'd array (caller frees packets + array). */
int zstr_st2022_5_fec_encode_matrix(zstr_st2022_5_fec_t *s, const AVPacket *media,
                                    AVPacket ***out_pkts, int *nb_out_pkts);
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
