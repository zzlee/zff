/*=============================================================================
    zstr_rtp_depayloader.c — RFC 3550 Generic RTP Depayloader Engine
=============================================================================*/
#include "zff/plugins/zstr_rtp.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

struct zstr_rtp_depayloader {
    zstr_rtp_codec_t codec;
    uint8_t payload_type;
    uint32_t clock_rate;

    uint8_t *au_buf;
    int au_size;
    int au_cap;
    uint32_t au_ts;
    int64_t au_pts;
    int64_t au_dts;
    AVRational time_base;

    /* Side Data from last received fragment */
    AVPacketSideData *saved_sd;
    int saved_sd_count;
};

zstr_rtp_depayloader_t* zstr_rtp_depayloader_create(zstr_rtp_codec_t codec,
                                                    uint8_t payload_type,
                                                    uint32_t clock_rate)
{
    zstr_rtp_depayloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->codec = codec;
    s->payload_type = payload_type ? payload_type : 96;
    s->clock_rate = clock_rate ? clock_rate : (codec == ZSTR_RTP_CODEC_AAC || codec == ZSTR_RTP_CODEC_PCM ? 48000 : 90000);
    s->time_base = (AVRational){ 1, (int)s->clock_rate };

    s->au_cap = 256 * 1024;
    s->au_buf = malloc(s->au_cap);
    return s;
}

static void append_bytes(zstr_rtp_depayloader_t *s, const uint8_t *data, int len)
{
    if (!data || len <= 0) return;
    if (s->au_size + len > s->au_cap) {
        s->au_cap = (s->au_size + len) * 2;
        s->au_buf = realloc(s->au_buf, s->au_cap);
    }
    memcpy(s->au_buf + s->au_size, data, len);
    s->au_size += len;
}

static void save_side_data(zstr_rtp_depayloader_t *s, const AVPacket *pkt)
{
    if (!pkt || pkt->side_data_elems <= 0) return;

    for (int i = 0; i < s->saved_sd_count; i++) {
        av_free(s->saved_sd[i].data);
    }
    free(s->saved_sd);
    s->saved_sd = NULL;
    s->saved_sd_count = 0;

    s->saved_sd = calloc(pkt->side_data_elems, sizeof(AVPacketSideData));
    if (!s->saved_sd) return;

    for (int i = 0; i < pkt->side_data_elems; i++) {
        const AVPacketSideData *sd = &pkt->side_data[i];
        s->saved_sd[i].type = sd->type;
        s->saved_sd[i].size = sd->size;
        s->saved_sd[i].data = av_malloc(sd->size);
        if (s->saved_sd[i].data) {
            memcpy(s->saved_sd[i].data, sd->data, sd->size);
        }
    }
    s->saved_sd_count = pkt->side_data_elems;
}

static void fill_output_packet(zstr_rtp_depayloader_t *s, AVPacket *out_pkt)
{
    av_new_packet(out_pkt, s->au_size);
    memcpy(out_pkt->data, s->au_buf, s->au_size);
    out_pkt->pts = s->au_pts;
    out_pkt->dts = s->au_dts;
    out_pkt->time_base = s->time_base;

    /* Restore side data */
    for (int i = 0; i < s->saved_sd_count; i++) {
        uint8_t *dst = av_packet_new_side_data(out_pkt, s->saved_sd[i].type, s->saved_sd[i].size);
        if (dst && s->saved_sd[i].data) {
            memcpy(dst, s->saved_sd[i].data, s->saved_sd[i].size);
        }
    }

    s->au_size = 0;
}

int zstr_rtp_depayloader_process(zstr_rtp_depayloader_t *s,
                                 const AVPacket *rtp_pkt,
                                 AVPacket *out_pkt,
                                 bool *ready)
{
    if (!s || !rtp_pkt || !out_pkt || !ready) return AVERROR(EINVAL);
    *ready = false;

    if (rtp_pkt->size < ZSTR_RTP_HEADER_LEN) return AVERROR_INVALIDDATA;

    const zstr_rtp_header_t *hdr = (const zstr_rtp_header_t*)rtp_pkt->data;
    bool marker = (hdr->m_pt & 0x80) != 0;
    uint32_t ts = ntohl(hdr->timestamp);

    const uint8_t *payload = rtp_pkt->data + ZSTR_RTP_HEADER_LEN;
    int payload_len = rtp_pkt->size - ZSTR_RTP_HEADER_LEN;
    if (payload_len <= 0) return 0;

    save_side_data(s, rtp_pkt);
    /* New timestamp with pending data means the previous AU never got its
     * marker (packet loss); drop the stale bytes and resync here. */
    if (s->au_size > 0 && ts != s->au_ts) s->au_size = 0;
    s->au_ts = ts;
    s->au_pts = rtp_pkt->pts;
    s->au_dts = rtp_pkt->dts;
    if (rtp_pkt->time_base.den > 0) s->time_base = rtp_pkt->time_base;

    static const uint8_t start_code[4] = { 0, 0, 0, 1 };

    if (s->codec == ZSTR_RTP_CODEC_H264) {
        uint8_t nal_type = payload[0] & 0x1F;

        if (nal_type == 28) {
            /* RFC 6184 FU-A */
            if (payload_len < 2) return AVERROR_INVALIDDATA;
            uint8_t fu_indicator = payload[0];
            uint8_t fu_header = payload[1];
            bool start = (fu_header & 0x80) != 0;
            bool end = (fu_header & 0x40) != 0;
            uint8_t orig_nal_type = fu_header & 0x1F;
            uint8_t nal_hdr = (fu_indicator & 0x60) | orig_nal_type;

            if (start) {
                s->au_size = 0;
                append_bytes(s, start_code, 4);
                append_bytes(s, &nal_hdr, 1);
            }
            append_bytes(s, payload + 2, payload_len - 2);

            if (end || marker) {
                fill_output_packet(s, out_pkt);
                *ready = true;
            }
        } else {
            /* Single NAL Unit: accumulate into the current access unit.
             * The payloader sets the marker bit only on the last RTP packet
             * of a multi-NAL access unit, so earlier NALs must be kept. */
            append_bytes(s, start_code, 4);
            append_bytes(s, payload, payload_len);
            if (marker) {
                fill_output_packet(s, out_pkt);
                *ready = true;
            }
        }
        return 0;
    } else if (s->codec == ZSTR_RTP_CODEC_AAC) {
        /* RFC 3640 MPEG4-GENERIC simple mode: AU-headers-length (16 bits)
         * + one AU-header + raw AAC frame. Fall back to raw passthrough for
         * senders that omit AU headers (pre-RFC3640 generic payloads). */
        const uint8_t *raw = payload;
        int raw_len = payload_len;
        if (payload_len >= 4) {
            int au_headers_bits = (payload[0] << 8) | payload[1];
            if (au_headers_bits == 16) {
                raw = payload + 4;
                raw_len = payload_len - 4;
            }
        }
        s->au_size = 0;
        append_bytes(s, raw, raw_len);
        if (marker) {
            fill_output_packet(s, out_pkt);
            *ready = true;
        }
        return 0;
    } else if (s->codec == ZSTR_RTP_CODEC_H265) {
        uint8_t nal_type = (payload[0] >> 1) & 0x3F;

        if (nal_type == 49) {
            /* RFC 7798 FU */
            if (payload_len < 3) return AVERROR_INVALIDDATA;
            uint8_t fu_hdr = payload[2];
            bool start = (fu_hdr & 0x80) != 0;
            bool end = (fu_hdr & 0x40) != 0;
            uint8_t orig_nal_type = fu_hdr & 0x3F;

            uint8_t n1 = ((orig_nal_type << 1) & 0x7E) | (payload[0] & 0x81);
            uint8_t n2 = payload[1];

            if (start) {
                s->au_size = 0;
                append_bytes(s, start_code, 4);
                append_bytes(s, &n1, 1);
                append_bytes(s, &n2, 1);
            }
            append_bytes(s, payload + 3, payload_len - 3);

            if (end || marker) {
                fill_output_packet(s, out_pkt);
                *ready = true;
            }
        } else {
            /* Single NAL Unit: accumulate like H.264 (see above). */
            append_bytes(s, start_code, 4);
            append_bytes(s, payload, payload_len);
            if (marker) {
                fill_output_packet(s, out_pkt);
                *ready = true;
            }
        }
        return 0;
    } else {
        /* Generic / AAC / PCM: Accumulate until marker bit */
        append_bytes(s, payload, payload_len);
        if (marker) {
            fill_output_packet(s, out_pkt);
            *ready = true;
        }
        return 0;
    }
}

void zstr_rtp_depayloader_free(zstr_rtp_depayloader_t **ps)
{
    if (!ps || !*ps) return;
    zstr_rtp_depayloader_t *s = *ps;

    for (int i = 0; i < s->saved_sd_count; i++) {
        av_free(s->saved_sd[i].data);
    }
    free(s->saved_sd);
    free(s->au_buf);
    free(s);
    *ps = NULL;
}
