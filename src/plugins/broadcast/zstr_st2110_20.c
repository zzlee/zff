/*=============================================================================
    zstr_st2110_20.c — SMPTE ST 2110-20 Video Payloader & Depayloader (RFC 4175)
=============================================================================*/
#include "zff/plugins/zstr_st2110_toolkit.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

struct zstr_st2110_20_payloader {
    int width;
    int height;
    enum AVPixelFormat fmt;
    uint8_t payload_type;
    uint32_t ssrc;
    uint32_t seq;
    int max_payload_bytes;
    int bpp;
};

zstr_st2110_20_payloader_t* zstr_st2110_20_payloader_create(const zstr_st2110_20_config_t *cfg)
{
    zstr_st2110_20_payloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->width = (cfg && cfg->width > 0) ? cfg->width : 1920;
    s->height = (cfg && cfg->height > 0) ? cfg->height : 1080;
    s->fmt = (cfg && cfg->fmt != AV_PIX_FMT_NONE) ? cfg->fmt : AV_PIX_FMT_UYVY422;
    s->payload_type = (cfg && cfg->payload_type) ? cfg->payload_type : 96;
    s->ssrc = (cfg && cfg->ssrc) ? cfg->ssrc : 0x21102001;
    s->seq = (uint32_t)(rand() & 0x7FFF);

    s->bpp = (s->fmt == AV_PIX_FMT_RGB24) ? 3 : 2; /* UYVY422 / YUV422 = 2 bytes/pixel */
    int max_p = (cfg && cfg->max_payload_bytes > 100) ? cfg->max_payload_bytes : 1376;
    s->max_payload_bytes = max_p - (max_p % s->bpp);

    return s;
}

int zstr_st2110_20_payloader_process(zstr_st2110_20_payloader_t *s,
                                     const AVFrame *frame,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts)
{
    if (!s || !frame || !out_pkts || !nb_out_pkts) return AVERROR(EINVAL);
    *out_pkts = NULL;
    *nb_out_pkts = 0;

    int w = frame->width > 0 ? frame->width : s->width;
    int h = frame->height > 0 ? frame->height : s->height;
    int bpp = s->bpp;

    /* Estimate packet count */
    int packets_per_line = (w * bpp + s->max_payload_bytes - 1) / s->max_payload_bytes;
    int est_packets = packets_per_line * h;
    AVPacket **pkts = calloc(est_packets + 4, sizeof(AVPacket*));
    if (!pkts) return AVERROR(ENOMEM);

    /* Derive 90kHz RTP timestamp from frame PTS */
    uint32_t ts;
    if (frame->pts != AV_NOPTS_VALUE) {
        if (frame->time_base.den > 0) {
            ts = (uint32_t)av_rescale_q(frame->pts, frame->time_base, (AVRational){1, 90000});
        } else {
            ts = (uint32_t)(frame->pts * 90000 / 1000000ULL);
        }
    } else {
        ts = (uint32_t)(s->seq * 3000);
    }

    int pkt_idx = 0;
    for (int y = 0; y < h; y++) {
        int x_offset_pixels = 0;
        int pixels_per_line = w;

        while (x_offset_pixels < pixels_per_line) {
            int remaining_px = pixels_per_line - x_offset_pixels;
            int max_px = s->max_payload_bytes / bpp;
            int px_to_send = remaining_px > max_px ? max_px : remaining_px;
            int bytes_to_send = px_to_send * bpp;

            bool is_last_packet = (y == h - 1) && ((x_offset_pixels + px_to_send) == pixels_per_line);

            AVPacket *pkt = av_packet_alloc();
            if (!pkt) {
                zstr_st2110_20_payloader_free_packets(pkts, pkt_idx);
                return AVERROR(ENOMEM);
            }
            int total_pkt_len = 12 + 2 + 6 + bytes_to_send;
            if (av_new_packet(pkt, total_pkt_len) < 0) {
                av_packet_free(&pkt);
                zstr_st2110_20_payloader_free_packets(pkts, pkt_idx);
                return AVERROR(ENOMEM);
            }

            uint8_t *data = pkt->data;
            /* 1. Standard RTP Header (12 bytes) */
            data[0] = 0x80; /* V=2, P=0, X=0, CC=0 */
            data[1] = (s->payload_type & 0x7F) | (is_last_packet ? 0x80 : 0x00);
            uint16_t rtp_seq = (uint16_t)(s->seq & 0xFFFF);
            data[2] = (uint8_t)(rtp_seq >> 8);
            data[3] = (uint8_t)(rtp_seq & 0xFF);
            data[4] = (uint8_t)(ts >> 24);
            data[5] = (uint8_t)((ts >> 16) & 0xFF);
            data[6] = (uint8_t)((ts >> 8) & 0xFF);
            data[7] = (uint8_t)(ts & 0xFF);
            data[8] = (uint8_t)(s->ssrc >> 24);
            data[9] = (uint8_t)((s->ssrc >> 16) & 0xFF);
            data[10] = (uint8_t)((s->ssrc >> 8) & 0xFF);
            data[11] = (uint8_t)(s->ssrc & 0xFF);

            /* 2. Extended Sequence Number (2 bytes) */
            uint16_t ext_seq = (uint16_t)((s->seq >> 16) & 0xFFFF);
            data[12] = (uint8_t)(ext_seq >> 8);
            data[13] = (uint8_t)(ext_seq & 0xFF);

            /* 3. RFC 4175 Line Header (6 bytes) */
            data[14] = (uint8_t)(bytes_to_send >> 8);
            data[15] = (uint8_t)(bytes_to_send & 0xFF);
            uint16_t f_line = (uint16_t)(y & 0x7FFF); /* F=0 */
            data[16] = (uint8_t)(f_line >> 8);
            data[17] = (uint8_t)(f_line & 0xFF);
            uint16_t c_offset = (uint16_t)(x_offset_pixels & 0x7FFF); /* C=0 */
            data[18] = (uint8_t)(c_offset >> 8);
            data[19] = (uint8_t)(c_offset & 0xFF);

            /* 4. Payload Data */
            const uint8_t *src_ptr = frame->data[0] + (y * frame->linesize[0]) + (x_offset_pixels * bpp);
            memcpy(data + 20, src_ptr, bytes_to_send);

            pkt->pts = frame->pts;
            pkt->time_base = (AVRational){ 1, 90000 };

            /* Propagate Frame Side Data (e.g. ZSTR_TAG_PTP) */
            for (int si = 0; si < frame->nb_side_data; si++) {
                const AVFrameSideData *sd = frame->side_data[si];
                uint8_t *dst_sd = av_packet_new_side_data(pkt, sd->type, sd->size);
                if (dst_sd) memcpy(dst_sd, sd->data, sd->size);
            }

            pkts[pkt_idx++] = pkt;
            s->seq++;
            x_offset_pixels += px_to_send;
        }
    }

    *out_pkts = pkts;
    *nb_out_pkts = pkt_idx;
    return 0;
}

void zstr_st2110_20_payloader_free_packets(AVPacket **pkts, int count)
{
    if (!pkts) return;
    for (int i = 0; i < count; i++) {
        if (pkts[i]) av_packet_free(&pkts[i]);
    }
    free(pkts);
}

void zstr_st2110_20_payloader_free(zstr_st2110_20_payloader_t **ps)
{
    if (!ps || !*ps) return;
    free(*ps);
    *ps = NULL;
}

/* ---------------------------------------------------------------------------
 * ST 2110-20 Depayloader
 * --------------------------------------------------------------------------- */
struct zstr_st2110_20_depayloader {
    int width;
    int height;
    enum AVPixelFormat fmt;
    int bpp;
    uint32_t cur_ts;
    AVFrame *cur_frame;
};

zstr_st2110_20_depayloader_t* zstr_st2110_20_depayloader_create(const zstr_st2110_20_config_t *cfg)
{
    zstr_st2110_20_depayloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->width = (cfg && cfg->width > 0) ? cfg->width : 1920;
    s->height = (cfg && cfg->height > 0) ? cfg->height : 1080;
    s->fmt = (cfg && cfg->fmt != AV_PIX_FMT_NONE) ? cfg->fmt : AV_PIX_FMT_UYVY422;
    s->bpp = (s->fmt == AV_PIX_FMT_RGB24) ? 3 : 2;

    return s;
}

int zstr_st2110_20_depayloader_process(zstr_st2110_20_depayloader_t *s,
                                       const AVPacket *rtp_pkt,
                                       AVFrame *out_frame,
                                       bool *ready)
{
    if (!s || !rtp_pkt || !out_frame || !ready) return AVERROR(EINVAL);
    *ready = false;

    if (rtp_pkt->size < 20) return AVERROR_INVALIDDATA;

    const uint8_t *data = rtp_pkt->data;
    bool marker = (data[1] & 0x80) != 0;
    uint32_t ts = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];

    /* Prepare accumulation frame */
    if (!s->cur_frame || s->cur_ts != ts) {
        if (s->cur_frame) av_frame_free(&s->cur_frame);

        s->cur_frame = av_frame_alloc();
        s->cur_frame->width = s->width;
        s->cur_frame->height = s->height;
        s->cur_frame->format = s->fmt;
        if (av_frame_get_buffer(s->cur_frame, 64) < 0) {
            av_frame_free(&s->cur_frame);
            return AVERROR(ENOMEM);
        }
        s->cur_frame->pts = rtp_pkt->pts;
        s->cur_frame->time_base = (AVRational){ 1, 90000 };
        s->cur_ts = ts;
    }

    /* Propagate packet side data to frame */
    for (int si = 0; si < rtp_pkt->side_data_elems; si++) {
        const AVPacketSideData *sd = &rtp_pkt->side_data[si];
        if (!av_frame_get_side_data(s->cur_frame, sd->type)) {
            AVFrameSideData *fsd = av_frame_new_side_data(s->cur_frame, sd->type, sd->size);
            if (fsd) memcpy(fsd->data, sd->data, sd->size);
        }
    }

    /* Parse Line Header(s) */
    int offset = 14;
    while (offset + 6 <= rtp_pkt->size) {
        uint16_t length = (data[offset] << 8) | data[offset + 1];
        uint16_t f_line = (data[offset + 2] << 8) | data[offset + 3];
        uint16_t c_off  = (data[offset + 4] << 8) | data[offset + 5];
        offset += 6;

        int line_no = f_line & 0x7FFF;
        int x_off   = c_off & 0x7FFF;

        if (offset + length <= rtp_pkt->size && line_no < s->height) {
            uint8_t *dst = s->cur_frame->data[0] + (line_no * s->cur_frame->linesize[0]) + (x_off * s->bpp);
            memcpy(dst, data + offset, length);
        }
        offset += length;
        break; /* Standard single line header per packet */
    }

    if (marker) {
        av_frame_ref(out_frame, s->cur_frame);
        av_frame_free(&s->cur_frame);
        *ready = true;
    }

    return 0;
}

void zstr_st2110_20_depayloader_free(zstr_st2110_20_depayloader_t **ps)
{
    if (!ps || !*ps) return;
    zstr_st2110_20_depayloader_t *s = *ps;
    if (s->cur_frame) av_frame_free(&s->cur_frame);
    free(s);
    *ps = NULL;
}
