/*=============================================================================
    zstr_st2110_22.c — SMPTE ST 2110-22 (JPEG XS over RTP, RFC 9134)

    v1 scope (documented):
    - Encode: AVFrame YUV422P 8-bit -> JPEG XS codestream (SVT-JPEG-XS).
    - RTP: codestream packetization mode only (K=0), progressive (I=00),
      sequential (T=1). Slice mode (K=1) is a follow-up.
    - Depacketize: reassembles by timestamp with SEP/P validation.
    - Decode: codestream -> AVFrame YUV422P via SVT frame-mode decoder.
    - zstreamer's st2110_22_payloader only encodes (no RTP, no decode);
      the framing here follows RFC 9134 directly.

    RFC 9134 payload header (4 bytes after the 12-byte RTP header):
      byte0: T(1) K(1) L(1) I(2) F-counter(5)
      byte1: SEP[10:3]
      byte2: SEP[2:0] P[10:8]
      byte3: P[7:0]
    M=1 on the last packet of the frame (== L=1 in codestream mode).
    90 kHz clock, dynamic payload type.
 =============================================================================*/
#include "zff/plugins/zstr_st2110_toolkit.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

#ifdef ZSTR_HAVE_SVT_JPEGXS
#include <SvtJpegxsEnc.h>
#include <SvtJpegxsDec.h>
#endif

#define ZSTR_XS_DEFAULT_PT 96
#define ZSTR_XS_DEFAULT_MTU 1400
#define ZSTR_XS_CLOCK 90000

/* --- Encoder --- */

struct zstr_st2110_22_encoder {
    int width;
    int height;
    int fps_num;
    int fps_den;
    int bpp_num;
    int bpp_den;
#ifdef ZSTR_HAVE_SVT_JPEGXS
    svt_jpeg_xs_encoder_api_t *enc;
#endif
    uint8_t *bitstream_buf;
    size_t bitstream_cap;
};

zstr_st2110_22_encoder_t *zstr_st2110_22_encoder_create(
    const zstr_st2110_22_config_t *cfg)
{
#ifndef ZSTR_HAVE_SVT_JPEGXS
    (void)cfg;
    return NULL;
#else
    zstr_st2110_22_encoder_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->width = (cfg && cfg->width > 0) ? cfg->width : 640;
    s->height = (cfg && cfg->height > 0) ? cfg->height : 480;
    s->fps_num = (cfg && cfg->fps_num > 0) ? cfg->fps_num : 60;
    s->fps_den = (cfg && cfg->fps_den > 0) ? cfg->fps_den : 1;
    s->bpp_num = (cfg && cfg->bpp_num > 0) ? cfg->bpp_num : 3;
    s->bpp_den = (cfg && cfg->bpp_den > 0) ? cfg->bpp_den : 1;

    s->enc = calloc(1, sizeof(svt_jpeg_xs_encoder_api_t));
    if (!s->enc) {
        free(s);
        return NULL;
    }
    if (svt_jpeg_xs_encoder_load_default_parameters(SVT_JPEGXS_API_VER_MAJOR,
                                                    SVT_JPEGXS_API_VER_MINOR,
                                                    s->enc) != 0) {
        free(s->enc);
        free(s);
        return NULL;
    }
    s->enc->source_width = (uint32_t)s->width;
    s->enc->source_height = (uint32_t)s->height;
    s->enc->bpp_numerator = (uint32_t)s->bpp_num;
    s->enc->bpp_denominator = (uint32_t)s->bpp_den;
    s->enc->input_bit_depth = 8;
    s->enc->colour_format = COLOUR_FORMAT_PLANAR_YUV422;
    if (svt_jpeg_xs_encoder_init(SVT_JPEGXS_API_VER_MAJOR,
                                 SVT_JPEGXS_API_VER_MINOR, s->enc) != 0) {
        free(s->enc);
        free(s);
        return NULL;
    }
    s->bitstream_cap = (size_t)s->width * s->height * 2;
    s->bitstream_buf = malloc(s->bitstream_cap);
    if (!s->bitstream_buf) {
        svt_jpeg_xs_encoder_close(s->enc);
        free(s->enc);
        free(s);
        return NULL;
    }
    return s;
#endif
}

int zstr_st2110_22_encode(zstr_st2110_22_encoder_t *s, const AVFrame *frame,
                          AVPacket **out_pkt)
{
    if (!s || !frame || !out_pkt) return AVERROR(EINVAL);
    *out_pkt = NULL;
#ifndef ZSTR_HAVE_SVT_JPEGXS
    return AVERROR(ENOSYS);
#else
    if (frame->format != AV_PIX_FMT_YUV422P || frame->width != s->width ||
        frame->height != s->height || !frame->data[0] || !frame->data[1] ||
        !frame->data[2])
        return AVERROR(EINVAL);

    svt_jpeg_xs_frame_t enc_in;
    memset(&enc_in, 0, sizeof(enc_in));
    enc_in.image.data_yuv[0] = frame->data[0];
    enc_in.image.alloc_size[0] = (uint32_t)(frame->linesize[0] * frame->height);
    enc_in.image.stride[0] = (uint32_t)frame->linesize[0];
    enc_in.image.data_yuv[1] = frame->data[1];
    enc_in.image.alloc_size[1] = (uint32_t)(frame->linesize[1] * frame->height / 2);
    enc_in.image.stride[1] = (uint32_t)frame->linesize[1];
    enc_in.image.data_yuv[2] = frame->data[2];
    enc_in.image.alloc_size[2] = (uint32_t)(frame->linesize[2] * frame->height / 2);
    enc_in.image.stride[2] = (uint32_t)frame->linesize[2];
    enc_in.bitstream.buffer = s->bitstream_buf;
    enc_in.bitstream.allocation_size = s->bitstream_cap;

    if (svt_jpeg_xs_encoder_send_picture(s->enc, &enc_in, 1) != SvtJxsErrorNone)
        return AVERROR(EIO);

    svt_jpeg_xs_frame_t enc_out;
    memset(&enc_out, 0, sizeof(enc_out));
    enc_out.bitstream.buffer = s->bitstream_buf;
    enc_out.bitstream.allocation_size = s->bitstream_cap;
    enc_out.bitstream.used_size = 0;
    SvtJxsErrorType_t err = svt_jpeg_xs_encoder_get_packet(s->enc, &enc_out, 1);
    if (err != SvtJxsErrorNone || enc_out.bitstream.used_size == 0)
        return AVERROR(EAGAIN);

    /* get_packet may return an internal buffer; copy it out */
    size_t used = enc_out.bitstream.used_size;
    const uint8_t *src = enc_out.bitstream.buffer;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, (int)used) < 0) {
        av_packet_free(&pkt);
        return AVERROR(ENOMEM);
    }
    memcpy(pkt->data, src, used);
    pkt->pts = frame->pts;
    pkt->dts = frame->pts;
    pkt->time_base = (AVRational){ 1, ZSTR_XS_CLOCK };
    *out_pkt = pkt;
    return 0;
#endif
}

void zstr_st2110_22_encoder_free(zstr_st2110_22_encoder_t **ps)
{
    if (!ps || !*ps) return;
    zstr_st2110_22_encoder_t *s = *ps;
#ifdef ZSTR_HAVE_SVT_JPEGXS
    if (s->enc) {
        svt_jpeg_xs_encoder_close(s->enc);
        free(s->enc);
    }
#endif
    free(s->bitstream_buf);
    free(s);
    *ps = NULL;
}

/* --- RFC 9134 codestream packetizer --- */

struct zstr_st2110_22_payloader {
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t seq;
    int mtu;
    uint8_t frame_counter; /* F counter mod 32 */
};

zstr_st2110_22_payloader_t *zstr_st2110_22_payloader_create(
    const zstr_st2110_22_config_t *cfg)
{
    zstr_st2110_22_payloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->payload_type = (cfg && cfg->payload_type) ? cfg->payload_type
                                                 : ZSTR_XS_DEFAULT_PT;
    s->ssrc = (cfg && cfg->ssrc) ? cfg->ssrc : 0x21102201;
    s->seq = (uint16_t)(rand() & 0xFFFF);
    s->mtu = (cfg && cfg->mtu >= 64 && cfg->mtu <= 9000) ? cfg->mtu
                                                         : ZSTR_XS_DEFAULT_MTU;
    return s;
}

static uint32_t xs_rtp_ts(const AVPacket *in)
{
    if (in->pts != AV_NOPTS_VALUE) {
        if (in->time_base.den > 0) {
            return (uint32_t)av_rescale_q(in->pts, in->time_base,
                                          (AVRational){ 1, ZSTR_XS_CLOCK });
        }
        return (uint32_t)(in->pts * ZSTR_XS_CLOCK / 1000000ULL);
    }
    return 0;
}

int zstr_st2110_22_payloader_process(zstr_st2110_22_payloader_t *s,
                                     const AVPacket *in,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts)
{
    if (!s || !in || !out_pkts || !nb_out_pkts) return AVERROR(EINVAL);
    *out_pkts = NULL;
    *nb_out_pkts = 0;
    if (!in->data || in->size <= 0) return 0;

    int max_payload = s->mtu - 12 - 4; /* RTP header + XS payload header */
    if (max_payload <= 0) return AVERROR(EINVAL);
    int total = (in->size + max_payload - 1) / max_payload;
    AVPacket **pkts = calloc(total, sizeof(AVPacket *));
    if (!pkts) return AVERROR(ENOMEM);

    uint32_t ts = xs_rtp_ts(in);
    uint8_t f = s->frame_counter;
    s->frame_counter = (uint8_t)((s->frame_counter + 1) & 0x1F);

    int offset = 0;
    int remaining = in->size;
    int idx = 0;
    while (remaining > 0) {
        int chunk = remaining > max_payload ? max_payload : remaining;
        bool is_last = (remaining == chunk);
        /* SEP overrun handling: P counts mod 2048, SEP counts overruns */
        int sep = (offset / max_payload) / 2048;
        int pcur = (offset / max_payload) % 2048;

        AVPacket *pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 12 + 4 + chunk) < 0) {
            av_packet_free(&pkt);
            zstr_st2110_22_payloader_free_packets(pkts, idx);
            return AVERROR(ENOMEM);
        }
        uint8_t *d = pkt->data;
        d[0] = 0x80;
        d[1] = (uint8_t)((is_last ? 0x80 : 0x00) | (s->payload_type & 0x7F));
        d[2] = (uint8_t)(s->seq >> 8);
        d[3] = (uint8_t)(s->seq & 0xFF);
        s->seq++;
        d[4] = (uint8_t)(ts >> 24);
        d[5] = (uint8_t)((ts >> 16) & 0xFF);
        d[6] = (uint8_t)((ts >> 8) & 0xFF);
        d[7] = (uint8_t)(ts & 0xFF);
        d[8] = (uint8_t)(s->ssrc >> 24);
        d[9] = (uint8_t)((s->ssrc >> 16) & 0xFF);
        d[10] = (uint8_t)((s->ssrc >> 8) & 0xFF);
        d[11] = (uint8_t)(s->ssrc & 0xFF);
        /* RFC 9134 payload header: T=1 K=0 L=M I=00 F SEP P */
        d[12] = (uint8_t)(0x80 | (is_last ? 0x20 : 0x00) | (f & 0x1F));
        d[13] = (uint8_t)((sep >> 3) & 0xFF);
        d[14] = (uint8_t)(((sep & 0x07) << 5) | ((pcur >> 8) & 0x1F));
        d[15] = (uint8_t)(pcur & 0xFF);
        memcpy(d + 16, in->data + offset, chunk);

        pkt->pts = in->pts;
        pkt->dts = in->dts;
        pkt->time_base = in->time_base;
        for (int i = 0; i < in->side_data_elems; i++) {
            const AVPacketSideData *sd = &in->side_data[i];
            uint8_t *dst = av_packet_new_side_data(pkt, sd->type, sd->size);
            if (dst) memcpy(dst, sd->data, sd->size);
        }
        pkts[idx++] = pkt;
        offset += chunk;
        remaining -= chunk;
    }

    *out_pkts = pkts;
    *nb_out_pkts = idx;
    return 0;
}

void zstr_st2110_22_payloader_free_packets(AVPacket **pkts, int count)
{
    if (!pkts) return;
    for (int i = 0; i < count; i++) {
        if (pkts[i]) av_packet_free(&pkts[i]);
    }
    free(pkts);
}

void zstr_st2110_22_payloader_free(zstr_st2110_22_payloader_t **ps)
{
    if (!ps || !*ps) return;
    free(*ps);
    *ps = NULL;
}

/* --- RFC 9134 depacketizer --- */

struct zstr_st2110_22_depayloader {
    uint8_t payload_type; /* 0 = accept any */
    uint8_t *accum;
    int accum_size;
    int accum_cap;
    uint32_t cur_ts;
    uint8_t cur_f;
    int expect_p; /* next P counter expected, -1 = any (first packet) */
    int have_frame;
};

zstr_st2110_22_depayloader_t *zstr_st2110_22_depayloader_create(
    const zstr_st2110_22_config_t *cfg)
{
    zstr_st2110_22_depayloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->payload_type = cfg ? cfg->payload_type : 0;
    s->accum_cap = 4 * 1024 * 1024;
    s->accum = malloc(s->accum_cap);
    if (!s->accum) {
        free(s);
        return NULL;
    }
    s->expect_p = -1;
    return s;
}

int zstr_st2110_22_depayloader_process(zstr_st2110_22_depayloader_t *s,
                                       const AVPacket *rtp_pkt,
                                       AVPacket *out_pkt,
                                       bool *ready)
{
    if (!s || !rtp_pkt || !out_pkt || !ready) return AVERROR(EINVAL);
    *ready = false;
    if (rtp_pkt->size < 16) return AVERROR_INVALIDDATA;

    const uint8_t *d = rtp_pkt->data;
    if ((d[0] >> 6) != 2) return AVERROR_INVALIDDATA;
    uint8_t pt = d[1] & 0x7F;
    if (s->payload_type && pt != s->payload_type) return AVERROR_INVALIDDATA;
    bool marker = (d[1] & 0x80) != 0;
    uint32_t ts = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
                  ((uint32_t)d[6] << 8) | d[7];

    /* Payload header */
    bool k = (d[12] >> 6) & 0x01;
    bool l = (d[12] >> 5) & 0x01;
    uint8_t f = d[12] & 0x1F;
    int sep = (d[13] << 3) | ((d[14] >> 5) & 0x07);
    int p = ((d[14] & 0x1F) << 8) | d[15];
    if (k != 0) return AVERROR_INVALIDDATA; /* v1: codestream mode only */
    if (sep != 0) return AVERROR_INVALIDDATA; /* v1: single-unit frames */

    if (s->have_frame && ts != s->cur_ts) {
        /* New frame before marker: previous frame was lost, resync */
        s->accum_size = 0;
        s->have_frame = 0;
        s->expect_p = -1;
    }
    if (!s->have_frame) {
        s->cur_ts = ts;
        s->cur_f = f;
        s->have_frame = 1;
        s->expect_p = 0;
    }
    if (p != s->expect_p) {
        /* Gap or reorder in v1 reassembly: drop the frame, resync here */
        s->accum_size = 0;
        s->have_frame = 1;
        s->cur_ts = ts;
        s->cur_f = f;
        if (p != 0) {
            s->expect_p = -1;
            s->have_frame = 0;
            return AVERROR_INVALIDDATA;
        }
        s->expect_p = 0;
    }

    int payload_len = rtp_pkt->size - 16;
    if (s->accum_size + payload_len > s->accum_cap) return AVERROR(ENOMEM);
    memcpy(s->accum + s->accum_size, d + 16, payload_len);
    s->accum_size += payload_len;
    s->expect_p = (p + 1) % 2048;

    /* In codestream mode L == M */
    if (l != (marker ? 1 : 0)) {
        s->accum_size = 0;
        s->have_frame = 0;
        s->expect_p = -1;
        return AVERROR_INVALIDDATA;
    }
    if (!marker) return 0;

    if (av_new_packet(out_pkt, s->accum_size) < 0) return AVERROR(ENOMEM);
    memcpy(out_pkt->data, s->accum, s->accum_size);
    out_pkt->pts = rtp_pkt->pts;
    out_pkt->dts = rtp_pkt->dts;
    out_pkt->time_base = rtp_pkt->time_base;
    s->accum_size = 0;
    s->have_frame = 0;
    s->expect_p = -1;
    *ready = true;
    return 0;
}

void zstr_st2110_22_depayloader_free(zstr_st2110_22_depayloader_t **ps)
{
    if (!ps || !*ps) return;
    free((*ps)->accum);
    free(*ps);
    *ps = NULL;
}

/* --- Decoder --- */

struct zstr_st2110_22_decoder {
    int width;
    int height;
#ifdef ZSTR_HAVE_SVT_JPEGXS
    svt_jpeg_xs_decoder_api_t *dec;
    svt_jpeg_xs_image_config_t img_cfg;
    int dec_inited;
#endif
};

zstr_st2110_22_decoder_t *zstr_st2110_22_decoder_create(
    const zstr_st2110_22_config_t *cfg)
{
    zstr_st2110_22_decoder_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->width = (cfg && cfg->width > 0) ? cfg->width : 640;
    s->height = (cfg && cfg->height > 0) ? cfg->height : 480;
    return s;
}

int zstr_st2110_22_decode(zstr_st2110_22_decoder_t *s, const AVPacket *in,
                          AVFrame **out_frame)
{
    if (!s || !in || !out_frame) return AVERROR(EINVAL);
    *out_frame = NULL;
#ifndef ZSTR_HAVE_SVT_JPEGXS
    return AVERROR(ENOSYS);
#else
    if (!in->data || in->size <= 0) return AVERROR(EINVAL);

    if (!s->dec_inited) {
        s->dec = calloc(1, sizeof(svt_jpeg_xs_decoder_api_t));
        if (!s->dec) return AVERROR(ENOMEM);
        memset(&s->img_cfg, 0, sizeof(s->img_cfg));
        if (svt_jpeg_xs_decoder_init(SVT_JPEGXS_API_VER_MAJOR,
                                     SVT_JPEGXS_API_VER_MINOR,
                                     s->dec, in->data, in->size,
                                     &s->img_cfg) != SvtJxsErrorNone) {
            free(s->dec);
            s->dec = NULL;
            return AVERROR(EINVAL);
        }
        s->dec_inited = 1;
    }

    svt_jpeg_xs_frame_t dec_in;
    memset(&dec_in, 0, sizeof(dec_in));
    dec_in.bitstream.buffer = (uint8_t *)in->data;
    dec_in.bitstream.allocation_size = (uint32_t)in->size;
    if (svt_jpeg_xs_decoder_send_frame(s->dec, &dec_in, 1) != SvtJxsErrorNone)
        return AVERROR(EIO);

    /* Output image buffers: YUV422P 8-bit sized from negotiated geometry */
    int w = (int)s->img_cfg.image_width;
    int h = (int)s->img_cfg.image_height;
    if (w <= 0 || h <= 0) {
        w = s->width;
        h = s->height;
    }
    AVFrame *frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    frame->width = w;
    frame->height = h;
    frame->format = AV_PIX_FMT_YUV422P;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

    svt_jpeg_xs_frame_t dec_out;
    memset(&dec_out, 0, sizeof(dec_out));
    dec_out.image.data_yuv[0] = frame->data[0];
    dec_out.image.alloc_size[0] = (uint32_t)(frame->linesize[0] * h);
    dec_out.image.stride[0] = (uint32_t)frame->linesize[0];
    dec_out.image.data_yuv[1] = frame->data[1];
    dec_out.image.alloc_size[1] = (uint32_t)(frame->linesize[1] * h / 2);
    dec_out.image.stride[1] = (uint32_t)frame->linesize[1];
    dec_out.image.data_yuv[2] = frame->data[2];
    dec_out.image.alloc_size[2] = (uint32_t)(frame->linesize[2] * h / 2);
    dec_out.image.stride[2] = (uint32_t)frame->linesize[2];

    if (svt_jpeg_xs_decoder_get_frame(s->dec, &dec_out, 1) != SvtJxsErrorNone) {
        av_frame_free(&frame);
        return AVERROR(EIO);
    }
    frame->pts = in->pts;
    *out_frame = frame;
    return 0;
#endif
}

void zstr_st2110_22_decoder_free(zstr_st2110_22_decoder_t **ps)
{
    if (!ps || !*ps) return;
    zstr_st2110_22_decoder_t *s = *ps;
#ifdef ZSTR_HAVE_SVT_JPEGXS
    if (s->dec) {
        svt_jpeg_xs_decoder_close(s->dec);
        free(s->dec);
    }
#endif
    free(s);
    *ps = NULL;
}
