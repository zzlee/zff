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
#include <stdint.h>

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

/* Split an Annex-B access unit into NAL units.
 * Returns NAL count (0 on OOM); falls back to a single "NAL" covering the
 * whole buffer when no start code is present (AVCC/raw input).
 * Out arrays have room for up to max_nals entries. */
static int split_annexb_nals(const uint8_t *data, int size,
                             const uint8_t **nals, int *lens, int max_nals)
{
    int nb = 0;
    int pos = 0;
    while (pos + 2 < size && nb < max_nals) {
        if (data[pos] == 0 && data[pos + 1] == 0) {
            int ns, clen;
            if (pos + 3 < size && data[pos + 2] == 1) { ns = pos + 3; clen = 3; }
            else if (pos + 4 < size && data[pos + 2] == 0 && data[pos + 3] == 1) { ns = pos + 4; clen = 4; }
            else { pos++; continue; }
            int ne = size;
            for (int j = ns; j + 3 < size; j++) {
                if (data[j] == 0 && data[j + 1] == 0 &&
                    (data[j + 2] == 1 ||
                     (j + 4 < size && data[j + 2] == 0 && data[j + 3] == 1))) {
                    ne = j;
                    break;
                }
            }
            if (ne - ns <= 0) { pos = ne; continue; }
            nals[nb] = data + ns;
            lens[nb] = ne - ns;
            nb++;
            pos = ne;
            (void)clen;
        } else {
            pos++;
        }
    }
    if (nb == 0 && size > 0) {
        nals[0] = data;
        lens[0] = size;
        nb = 1;
    }
    return nb;
}

/* Growable RTP packet list used by multi-NAL packetization */
typedef struct {
    AVPacket **pkts;
    int count;
    int cap;
} rtp_pkt_list_t;

static int rtp_list_append(rtp_pkt_list_t *l, AVPacket *pkt)
{
    if (l->count >= l->cap) {
        int ncap = l->cap ? l->cap * 2 : 8;
        AVPacket **np = realloc(l->pkts, ncap * sizeof(AVPacket *));
        if (!np) return -1;
        l->pkts = np;
        l->cap = ncap;
    }
    l->pkts[l->count++] = pkt;
    return 0;
}

static void rtp_list_cleanup(rtp_pkt_list_t *l)
{
    if (!l->pkts) return;
    for (int i = 0; i < l->count; i++) {
        if (l->pkts[i]) av_packet_free(&l->pkts[i]);
    }
    free(l->pkts);
    l->pkts = NULL;
    l->count = l->cap = 0;
}

/* Packetize one H.264 NAL (single or FU-A) and append to list */
static int packetize_h264_nal(zstr_rtp_payloader_t *s, const uint8_t *nal, int nal_len,
                              bool is_last, uint32_t rtp_ts, const AVPacket *src,
                              rtp_pkt_list_t *out)
{
    int max_payload = s->mtu - ZSTR_RTP_HEADER_LEN;
    if (nal_len <= max_payload) {
        AVPacket *pkt = create_rtp_packet(s, is_last, rtp_ts, nal, nal_len, src);
        if (!pkt) return AVERROR(ENOMEM);
        if (rtp_list_append(out, pkt) < 0) {
            av_packet_free(&pkt);
            return AVERROR(ENOMEM);
        }
        return 0;
    }

    uint8_t nal_header = nal[0];
    uint8_t fu_indicator = (nal_header & 0x60) | 28;
    uint8_t nal_type = nal_header & 0x1F;
    const uint8_t *payload = nal + 1;
    int remaining = nal_len - 1;
    int fu_max = max_payload - 2;
    int offset = 0;
    while (remaining > 0) {
        int chunk = remaining > fu_max ? fu_max : remaining;
        bool first = (offset == 0);
        bool last = (remaining == chunk);
        uint8_t fu_header = nal_type;
        if (first) fu_header |= 0x80;
        if (last)  fu_header |= 0x40;
        /* assemble 2-byte FU header + chunk via temp buffer */
        uint8_t *fbuf = malloc(2 + chunk);
        if (!fbuf) return AVERROR(ENOMEM);
        fbuf[0] = fu_indicator;
        fbuf[1] = fu_header;
        memcpy(fbuf + 2, payload + offset, chunk);
        AVPacket *pkt = create_rtp_packet(s, last && is_last, rtp_ts, fbuf, 2 + chunk, src);
        free(fbuf);
        if (!pkt) return AVERROR(ENOMEM);
        if (rtp_list_append(out, pkt) < 0) {
            av_packet_free(&pkt);
            return AVERROR(ENOMEM);
        }
        offset += chunk;
        remaining -= chunk;
    }
    return 0;
}

/* Packetize one H.265 NAL (single or FU type 49) and append to list */
static int packetize_h265_nal(zstr_rtp_payloader_t *s, const uint8_t *nal, int nal_len,
                              bool is_last, uint32_t rtp_ts, const AVPacket *src,
                              rtp_pkt_list_t *out)
{
    int max_payload = s->mtu - ZSTR_RTP_HEADER_LEN;
    if (nal_len <= max_payload) {
        AVPacket *pkt = create_rtp_packet(s, is_last, rtp_ts, nal, nal_len, src);
        if (!pkt) return AVERROR(ENOMEM);
        if (rtp_list_append(out, pkt) < 0) {
            av_packet_free(&pkt);
            return AVERROR(ENOMEM);
        }
        return 0;
    }

    uint8_t nal_type = (nal[0] >> 1) & 0x3F;
    uint8_t fu_hdr0 = ((49 << 1) & 0x7E) | (nal[0] & 0x81);
    uint8_t fu_hdr1 = nal[1];
    const uint8_t *payload = nal + 2;
    int remaining = nal_len - 2;
    int fu_max = max_payload - 3;
    int offset = 0;
    while (remaining > 0) {
        int chunk = remaining > fu_max ? fu_max : remaining;
        bool first = (offset == 0);
        bool last = (remaining == chunk);
        uint8_t fu_header = nal_type;
        if (first) fu_header |= 0x80;
        if (last)  fu_header |= 0x40;
        uint8_t *fbuf = malloc(3 + chunk);
        if (!fbuf) return AVERROR(ENOMEM);
        fbuf[0] = fu_hdr0;
        fbuf[1] = fu_hdr1;
        fbuf[2] = fu_header;
        memcpy(fbuf + 3, payload + offset, chunk);
        AVPacket *pkt = create_rtp_packet(s, last && is_last, rtp_ts, fbuf, 3 + chunk, src);
        free(fbuf);
        if (!pkt) return AVERROR(ENOMEM);
        if (rtp_list_append(out, pkt) < 0) {
            av_packet_free(&pkt);
            return AVERROR(ENOMEM);
        }
        offset += chunk;
        remaining -= chunk;
    }
    return 0;
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

    if (s->codec == ZSTR_RTP_CODEC_H264 || s->codec == ZSTR_RTP_CODEC_H265) {
        /* Split Annex-B access units into NALs; marker bit is set only on
         * the last RTP packet of the whole access unit (RFC 6184/7798). */
        const uint8_t *nals[128];
        int lens[128];
        int nb_nals = split_annexb_nals(data, size, nals, lens, 128);
        if (nb_nals <= 0) return 0;

        rtp_pkt_list_t list = { 0 };
        int ret = 0;
        for (int i = 0; i < nb_nals && ret == 0; i++) {
            bool is_last = (i == nb_nals - 1);
            if (s->codec == ZSTR_RTP_CODEC_H264)
                ret = packetize_h264_nal(s, nals[i], lens[i], is_last, rtp_ts, in, &list);
            else
                ret = packetize_h265_nal(s, nals[i], lens[i], is_last, rtp_ts, in, &list);
        }
        if (ret < 0) {
            rtp_list_cleanup(&list);
            return ret;
        }
        *out_pkts = list.pkts;
        *nb_out_pkts = list.count;
        return 0;
    } else if (s->codec == ZSTR_RTP_CODEC_AAC) {
        /* RFC 3640 MPEG4-GENERIC, simple mode: AU-headers-length (16 bits)
         * + one AU-header (13-bit size + 3-bit index) + raw AAC frame.
         * Matches the SDP fmtp (sizelength=13;indexlength=3;indexdeltalength=3). */
        const int au_overhead = 4;
        int frag_max = max_payload - au_overhead;
        if (frag_max <= 0) return AVERROR(EINVAL);

        int total_frags = (size + frag_max - 1) / frag_max;
        AVPacket **pkts = calloc(total_frags, sizeof(AVPacket *));
        if (!pkts) return AVERROR(ENOMEM);

        int frag_idx = 0;
        int offset = 0;
        int remaining = size;
        while (remaining > 0) {
            int chunk = remaining > frag_max ? frag_max : remaining;
            bool is_last = (remaining == chunk);
            uint8_t *fbuf = malloc(au_overhead + chunk);
            if (!fbuf) {
                zstr_rtp_payloader_free_packets(pkts, frag_idx);
                return AVERROR(ENOMEM);
            }
            fbuf[0] = 0;
            fbuf[1] = 16; /* AU-headers-length in bits */
            uint16_t au = (uint16_t)(chunk << 3);
            fbuf[2] = (uint8_t)(au >> 8);
            fbuf[3] = (uint8_t)(au & 0xff);
            memcpy(fbuf + au_overhead, data + offset, chunk);

            AVPacket *pkt = create_rtp_packet(s, is_last, rtp_ts, fbuf, au_overhead + chunk, in);
            free(fbuf);
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

uint16_t zstr_rtp_payloader_seq(const zstr_rtp_payloader_t *s)
{
    return s ? s->seq : 0;
}
