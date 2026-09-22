/*=============================================================================
    zstr_st2110_40.c — SMPTE ST 2110-40 Ancillary Data Payloader & Depayloader

    zff profile wire format (documented; ST 344 8-bit mapping):
    - Input AVPacket carries one or more ANC packets, each laid out as
        DID(1) SDID(1) DC(1) UDW[DC] CS(1)
      where CS is the 8-bit sum of DID..UDW[last] modulo 256. (SD 10-bit
      words are conveyed in their 8-bit payload octet form, as most
      software ANC stacks exchange them.)
    - Payloader chunks the ANC byte stream into RTP packets (12-byte
      header, PT configurable, default 100, 90 kHz clock); marker bit is
      set only on the last packet of each input set.
    - Depayloader accumulates by RTP timestamp until marker, validates
      per-packet DID/DC bounds, and emits the concatenated ANC stream.

    Ported from zstreamer st2110_40_payloader (opaque framing kept), with
    ANC packet validation added on the depayload path.
 =============================================================================*/
#include "zff/plugins/zstr_st2110_toolkit.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

#define ZSTR_ST2110_40_DEFAULT_PT 100
#define ZSTR_ST2110_40_DEFAULT_MTU 1400
#define ZSTR_ST2110_40_CLOCK 90000

struct zstr_st2110_40_payloader {
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t seq;
    int mtu;
};

struct zstr_st2110_40_depayloader {
    uint8_t payload_type; /* 0 = accept any */
    uint8_t *accum;
    int accum_size;
    int accum_cap;
    uint32_t cur_ts;
    int64_t cur_pts;
    AVRational cur_tb;
};

/* --- Payloader --- */

zstr_st2110_40_payloader_t *zstr_st2110_40_payloader_create(
    const zstr_st2110_40_config_t *cfg)
{
    zstr_st2110_40_payloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->payload_type = (cfg && cfg->payload_type) ? cfg->payload_type
                                                 : ZSTR_ST2110_40_DEFAULT_PT;
    s->ssrc = (cfg && cfg->ssrc) ? cfg->ssrc : 0x21104000;
    s->seq = (uint16_t)(rand() & 0xFFFF);
    s->mtu = (cfg && cfg->mtu >= 64 && cfg->mtu <= 9000) ? cfg->mtu
                                                         : ZSTR_ST2110_40_DEFAULT_MTU;
    return s;
}

static uint32_t anc_rtp_ts(const AVPacket *in)
{
    if (in->pts != AV_NOPTS_VALUE) {
        if (in->time_base.den > 0) {
            return (uint32_t)av_rescale_q(in->pts, in->time_base,
                                          (AVRational){ 1, ZSTR_ST2110_40_CLOCK });
        }
        return (uint32_t)(in->pts * ZSTR_ST2110_40_CLOCK / 1000000ULL);
    }
    return 0;
}

int zstr_st2110_40_payloader_process(zstr_st2110_40_payloader_t *s,
                                     const AVPacket *in,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts)
{
    if (!s || !in || !out_pkts || !nb_out_pkts) return AVERROR(EINVAL);
    *out_pkts = NULL;
    *nb_out_pkts = 0;
    if (!in->data || in->size <= 0) return 0;

    int max_payload = s->mtu - 12;
    int total = (in->size + max_payload - 1) / max_payload;
    AVPacket **pkts = calloc(total, sizeof(AVPacket *));
    if (!pkts) return AVERROR(ENOMEM);

    uint32_t ts = anc_rtp_ts(in);
    int offset = 0;
    int remaining = in->size;
    int idx = 0;
    while (remaining > 0) {
        int chunk = remaining > max_payload ? max_payload : remaining;
        bool is_last = (remaining == chunk);
        AVPacket *pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 12 + chunk) < 0) {
            av_packet_free(&pkt);
            zstr_st2110_40_payloader_free_packets(pkts, idx);
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
        memcpy(d + 12, in->data + offset, chunk);

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

void zstr_st2110_40_payloader_free_packets(AVPacket **pkts, int count)
{
    if (!pkts) return;
    for (int i = 0; i < count; i++) {
        if (pkts[i]) av_packet_free(&pkts[i]);
    }
    free(pkts);
}

void zstr_st2110_40_payloader_free(zstr_st2110_40_payloader_t **ps)
{
    if (!ps || !*ps) return;
    free(*ps);
    *ps = NULL;
}

/* --- Depayloader --- */

zstr_st2110_40_depayloader_t *zstr_st2110_40_depayloader_create(
    const zstr_st2110_40_config_t *cfg)
{
    zstr_st2110_40_depayloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->payload_type = cfg ? cfg->payload_type : 0; /* 0 = accept any */
    s->accum_cap = 65536;
    s->accum = malloc(s->accum_cap);
    if (!s->accum) {
        free(s);
        return NULL;
    }
    return s;
}

/* Validate ANC packet bounds: DID SDID DC UDW[DC] CS. Returns total packet
 * length, or -1 when truncated/invalid. */
static int anc_packet_len(const uint8_t *data, int size)
{
    if (size < 4) return -1;
    int dc = data[2];
    int total = 3 + dc + 1;
    if (total > size) return -1;
    if (data[0] < 0x04) return -1; /* DID 0x00-0x03 reserved */
    return total;
}

int zstr_st2110_40_depayloader_process(zstr_st2110_40_depayloader_t *s,
                                       const AVPacket *rtp_pkt,
                                       AVPacket *out_pkt,
                                       bool *ready)
{
    if (!s || !rtp_pkt || !out_pkt || !ready) return AVERROR(EINVAL);
    *ready = false;
    if (rtp_pkt->size < 12) return AVERROR_INVALIDDATA;

    const uint8_t *d = rtp_pkt->data;
    if ((d[0] >> 6) != 2) return AVERROR_INVALIDDATA;
    uint8_t pt = d[1] & 0x7F;
    if (s->payload_type && pt != s->payload_type) return AVERROR_INVALIDDATA;
    bool marker = (d[1] & 0x80) != 0;
    uint32_t ts = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
                  ((uint32_t)d[6] << 8) | d[7];

    const uint8_t *payload = d + 12;
    int payload_len = rtp_pkt->size - 12;

    /* New timestamp with pending data: resync (previous set lost its marker) */
    if (s->accum_size > 0 && ts != s->cur_ts) s->accum_size = 0;
    s->cur_ts = ts;
    s->cur_pts = rtp_pkt->pts;
    s->cur_tb = rtp_pkt->time_base;

    /* Fragmentation is byte-level: only a fragment that STARTS a set must
     * begin on an ANC packet header (DID/SDID/DC present); its full length
     * need not fit this fragment. Mid-set fragments append blindly; the
     * reassembled set is fully validated at marker time. */
    if (s->accum_size == 0) {
        if (payload_len < 3 || payload[0] < 0x04)
            return AVERROR_INVALIDDATA;
    }

    if (s->accum_size + payload_len > s->accum_cap) return AVERROR(ENOMEM);
    memcpy(s->accum + s->accum_size, payload, payload_len);
    s->accum_size += payload_len;

    if (!marker) return 0;

    /* Final validation over the reassembled set */
    int pos = 0;
    while (pos < s->accum_size) {
        int plen = anc_packet_len(s->accum + pos, s->accum_size - pos);
        if (plen < 0) {
            s->accum_size = 0;
            return AVERROR_INVALIDDATA;
        }
        pos += plen;
    }

    if (av_new_packet(out_pkt, s->accum_size) < 0) return AVERROR(ENOMEM);
    memcpy(out_pkt->data, s->accum, s->accum_size);
    out_pkt->pts = s->cur_pts;
    out_pkt->dts = s->cur_pts;
    out_pkt->time_base = s->cur_tb;
    s->accum_size = 0;
    *ready = true;
    return 0;
}

void zstr_st2110_40_depayloader_free(zstr_st2110_40_depayloader_t **ps)
{
    if (!ps || !*ps) return;
    free((*ps)->accum);
    free(*ps);
    *ps = NULL;
}
