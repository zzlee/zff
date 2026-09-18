/*=============================================================================
    zstr_rtp_payloader.c — RFC 3550 Generic RTP Payloader Engine
=============================================================================*/
#include "zff/plugins/zstr_rtp.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

struct zstr_rtp_payloader {
    zstr_rtp_codec_t codec;
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t clock_rate;
    int mtu;
};

zstr_rtp_payloader_t* zstr_rtp_payloader_create(zstr_rtp_codec_t codec,
                                                uint8_t payload_type,
                                                uint32_t ssrc,
                                                uint32_t clock_rate,
                                                int mtu)
{
    zstr_rtp_payloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->codec = codec;
    s->payload_type = payload_type ? payload_type : 96;
    s->ssrc = ssrc ? ssrc : 0x12345678;
    s->seq = (uint16_t)(rand() & 0x7FFF);
    s->clock_rate = clock_rate ? clock_rate : (codec == ZSTR_RTP_CODEC_AAC || codec == ZSTR_RTP_CODEC_PCM ? 48000 : 90000);
    s->mtu = (mtu >= 100 && mtu <= 65500) ? mtu : ZSTR_RTP_DEFAULT_MTU;

    return s;
}

static AVPacket* create_rtp_packet(zstr_rtp_payloader_t *s,
                                   bool marker,
                                   uint32_t rtp_ts,
                                   const uint8_t *payload,
                                   int payload_len,
                                   const AVPacket *src_pkt)
{
    int total_len = ZSTR_RTP_HEADER_LEN + payload_len;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return NULL;

    if (av_new_packet(pkt, total_len) < 0) {
        av_packet_free(&pkt);
        return NULL;
    }

    zstr_rtp_header_t *hdr = (zstr_rtp_header_t*)pkt->data;
    hdr->v_p_x_cc = 0x80; /* V=2, P=0, X=0, CC=0 */
    hdr->m_pt = (uint8_t)((marker ? 0x80 : 0x00) | (s->payload_type & 0x7F));
    hdr->seq = htons(s->seq++);
    hdr->timestamp = htonl(rtp_ts);
    hdr->ssrc = htonl(s->ssrc);

    if (payload_len > 0 && payload) {
        memcpy(pkt->data + ZSTR_RTP_HEADER_LEN, payload, payload_len);
    }

    if (src_pkt) {
        pkt->pts = src_pkt->pts;
        pkt->dts = src_pkt->dts;
        pkt->duration = src_pkt->duration;
        pkt->time_base = src_pkt->time_base;

        /* Propagate Side Data (including PTP / Hardware FourCC tags) */
        for (int i = 0; i < src_pkt->side_data_elems; i++) {
            const AVPacketSideData *sd = &src_pkt->side_data[i];
            uint8_t *dst_sd = av_packet_new_side_data(pkt, sd->type, sd->size);
            if (dst_sd) {
                memcpy(dst_sd, sd->data, sd->size);
            }
        }
    }

    return pkt;
}

/* Helper to strip Annex-B start code */
static const uint8_t* strip_start_code(const uint8_t *p, int *len)
{
    if (*len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        *len -= 4;
        return p + 4;
    }
    if (*len >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        *len -= 3;
        return p + 3;
    }
    return p;
}

int zstr_rtp_payloader_process(zstr_rtp_payloader_t *s,
                               const AVPacket *in,
                               AVPacket ***out_pkts,
                               int *nb_out_pkts)
{
    if (!s || !in || !out_pkts || !nb_out_pkts) return AVERROR(EINVAL);
    *out_pkts = NULL;
    *nb_out_pkts = 0;

    if (!in->data || in->size <= 0) return 0;

    /* Compute RTP timestamp */
    uint32_t rtp_ts;
    if (in->pts != AV_NOPTS_VALUE) {
        if (in->time_base.den > 0) {
            rtp_ts = (uint32_t)av_rescale_q(in->pts, in->time_base, (AVRational){1, (int)s->clock_rate});
        } else {
            rtp_ts = (uint32_t)(in->pts * s->clock_rate / 1000000ULL);
        }
    } else {
        rtp_ts = (uint32_t)(s->seq * 3000);
    }

    int max_payload = s->mtu - ZSTR_RTP_HEADER_LEN;
    const uint8_t *data = in->data;
    int size = in->size;

    if (s->codec == ZSTR_RTP_CODEC_H264) {
        data = strip_start_code(data, &size);
        if (size <= 0) return 0;

        if (size <= max_payload) {
            /* Single NAL unit packet */
            AVPacket *pkt = create_rtp_packet(s, true, rtp_ts, data, size, in);
            if (!pkt) return AVERROR(ENOMEM);
            *out_pkts = malloc(sizeof(AVPacket*));
            (*out_pkts)[0] = pkt;
            *nb_out_pkts = 1;
            return 0;
        }

        /* FU-A Fragmentation */
        uint8_t nal_header = data[0];
        uint8_t fu_indicator = (nal_header & 0x60) | 28; /* NRI | FU-A type 28 */
        uint8_t nal_type = nal_header & 0x1F;

        const uint8_t *nal_payload = data + 1;
        int remaining = size - 1;
        int fu_max_payload = max_payload - 2; /* 1 byte FU indicator + 1 byte FU header */

        int total_frags = (remaining + fu_max_payload - 1) / fu_max_payload;
        AVPacket **pkts = calloc(total_frags, sizeof(AVPacket*));
        if (!pkts) return AVERROR(ENOMEM);

        int frag_idx = 0;
        int offset = 0;
        while (remaining > 0) {
            int chunk = remaining > fu_max_payload ? fu_max_payload : remaining;
            bool is_first = (offset == 0);
            bool is_last = (remaining == chunk);

            uint8_t fu_header = nal_type;
            if (is_first) fu_header |= 0x80; /* Start bit S=1 */
            if (is_last)  fu_header |= 0x40; /* End bit E=1 */

            int rtp_frag_len = 2 + chunk;
            uint8_t *frag_buf = malloc(rtp_frag_len);
            frag_buf[0] = fu_indicator;
            frag_buf[1] = fu_header;
            memcpy(frag_buf + 2, nal_payload + offset, chunk);

            AVPacket *pkt = create_rtp_packet(s, is_last, rtp_ts, frag_buf, rtp_frag_len, in);
            free(frag_buf);
            if (!pkt) {
                zstr_rtp_payloader_free_packets(pkts, frag_idx);
                return AVERROR(ENOMEM);
            }

            pkts[frag_idx++] = pkt;
            offset += chunk;
            remaining -= chunk;
        }

        *out_pkts = pkts;
        *nb_out_pkts = frag_idx;
        return 0;
    } else if (s->codec == ZSTR_RTP_CODEC_H265) {
        data = strip_start_code(data, &size);
        if (size <= 0) return 0;

        if (size <= max_payload) {
            AVPacket *pkt = create_rtp_packet(s, true, rtp_ts, data, size, in);
            if (!pkt) return AVERROR(ENOMEM);
            *out_pkts = malloc(sizeof(AVPacket*));
            (*out_pkts)[0] = pkt;
            *nb_out_pkts = 1;
            return 0;
        }

        /* H.265 FU Fragmentation (RFC 7798 type 49) */
        uint8_t nal_type = (data[0] >> 1) & 0x3F;
        uint8_t fu_hdr0 = ((49 << 1) & 0x7E) | (data[0] & 0x81);
        uint8_t fu_hdr1 = data[1];

        const uint8_t *nal_payload = data + 2;
        int remaining = size - 2;
        int fu_max_payload = max_payload - 3; /* 2 bytes PayloadHdr + 1 byte FU header */

        int total_frags = (remaining + fu_max_payload - 1) / fu_max_payload;
        AVPacket **pkts = calloc(total_frags, sizeof(AVPacket*));
        if (!pkts) return AVERROR(ENOMEM);

        int frag_idx = 0;
        int offset = 0;
        while (remaining > 0) {
            int chunk = remaining > fu_max_payload ? fu_max_payload : remaining;
            bool is_first = (offset == 0);
            bool is_last = (remaining == chunk);

            uint8_t fu_header = nal_type;
            if (is_first) fu_header |= 0x80;
            if (is_last)  fu_header |= 0x40;

            int rtp_frag_len = 3 + chunk;
            uint8_t *frag_buf = malloc(rtp_frag_len);
            frag_buf[0] = fu_hdr0;
            frag_buf[1] = fu_hdr1;
            frag_buf[2] = fu_header;
            memcpy(frag_buf + 3, nal_payload + offset, chunk);

            AVPacket *pkt = create_rtp_packet(s, is_last, rtp_ts, frag_buf, rtp_frag_len, in);
            free(frag_buf);
            if (!pkt) {
                zstr_rtp_payloader_free_packets(pkts, frag_idx);
                return AVERROR(ENOMEM);
            }

            pkts[frag_idx++] = pkt;
            offset += chunk;
            remaining -= chunk;
        }

        *out_pkts = pkts;
        *nb_out_pkts = frag_idx;
        return 0;
    } else {
        /* Generic / AAC / PCM: Chunk up to max_payload */
        int total_frags = (size + max_payload - 1) / max_payload;
        AVPacket **pkts = calloc(total_frags, sizeof(AVPacket*));
        if (!pkts) return AVERROR(ENOMEM);

        int frag_idx = 0;
        int offset = 0;
        int remaining = size;
        while (remaining > 0) {
            int chunk = remaining > max_payload ? max_payload : remaining;
            bool is_last = (remaining == chunk);

            AVPacket *pkt = create_rtp_packet(s, is_last, rtp_ts, data + offset, chunk, in);
            if (!pkt) {
                zstr_rtp_payloader_free_packets(pkts, frag_idx);
                return AVERROR(ENOMEM);
            }

            pkts[frag_idx++] = pkt;
            offset += chunk;
            remaining -= chunk;
        }

        *out_pkts = pkts;
        *nb_out_pkts = frag_idx;
        return 0;
    }
}

void zstr_rtp_payloader_free_packets(AVPacket **pkts, int count)
{
    if (!pkts) return;
    for (int i = 0; i < count; i++) {
        if (pkts[i]) av_packet_free(&pkts[i]);
    }
    free(pkts);
}

void zstr_rtp_payloader_free(zstr_rtp_payloader_t **ps)
{
    if (!ps || !*ps) return;
    free(*ps);
    *ps = NULL;
}
