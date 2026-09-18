/*=============================================================================
    zstr_st2110_30.c — SMPTE ST 2110-30 PCM Audio Payloader & Depayloader (AES67)
=============================================================================*/
#include "zff/plugins/zstr_st2110_toolkit.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

struct zstr_st2110_30_payloader {
    int channels;
    int sample_rate;
    int bit_depth;
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t seq;
    int packet_time_us;
    int samples_per_packet;
};

zstr_st2110_30_payloader_t* zstr_st2110_30_payloader_create(const zstr_st2110_30_config_t *cfg)
{
    zstr_st2110_30_payloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->channels = (cfg && cfg->channels > 0) ? cfg->channels : 2;
    s->sample_rate = (cfg && cfg->sample_rate > 0) ? cfg->sample_rate : 48000;
    s->bit_depth = (cfg && cfg->bit_depth > 0) ? cfg->bit_depth : 16;
    s->payload_type = (cfg && cfg->payload_type) ? cfg->payload_type : 97;
    s->ssrc = (cfg && cfg->ssrc) ? cfg->ssrc : 0x21103001;
    s->seq = (uint16_t)(rand() & 0x7FFF);
    s->packet_time_us = (cfg && cfg->packet_time_us > 0) ? cfg->packet_time_us : 1000; /* 1ms default */

    s->samples_per_packet = (s->sample_rate * s->packet_time_us) / 1000000;
    if (s->samples_per_packet <= 0) s->samples_per_packet = 48;

    return s;
}

int zstr_st2110_30_payloader_process(zstr_st2110_30_payloader_t *s,
                                     const AVFrame *frame,
                                     AVPacket ***out_pkts,
                                     int *nb_out_pkts)
{
    if (!s || !frame || !out_pkts || !nb_out_pkts) return AVERROR(EINVAL);
    *out_pkts = NULL;
    *nb_out_pkts = 0;

    int nb_samples = frame->nb_samples;
    int bytes_per_sample = s->bit_depth / 8;
    int frame_stride = s->channels * bytes_per_sample;
    int chunk_samples = s->samples_per_packet;
    int chunk_bytes = chunk_samples * frame_stride;

    int total_pkts = (nb_samples + chunk_samples - 1) / chunk_samples;
    AVPacket **pkts = calloc(total_pkts, sizeof(AVPacket*));
    if (!pkts) return AVERROR(ENOMEM);

    uint32_t base_ts;
    if (frame->pts != AV_NOPTS_VALUE) {
        if (frame->time_base.den > 0) {
            base_ts = (uint32_t)av_rescale_q(frame->pts, frame->time_base, (AVRational){1, s->sample_rate});
        } else {
            base_ts = (uint32_t)(frame->pts * s->sample_rate / 1000000ULL);
        }
    } else {
        base_ts = (uint32_t)(s->seq * 48);
    }

    const uint8_t *src = frame->data[0];
    int remaining = nb_samples;
    int pkt_idx = 0;
    int sample_offset = 0;

    while (remaining > 0) {
        int n = remaining > chunk_samples ? chunk_samples : remaining;
        int payload_len = n * frame_stride;
        bool is_last = (remaining == n);

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            zstr_st2110_20_payloader_free_packets(pkts, pkt_idx);
            return AVERROR(ENOMEM);
        }
        if (av_new_packet(pkt, 12 + payload_len) < 0) {
            av_packet_free(&pkt);
            zstr_st2110_20_payloader_free_packets(pkts, pkt_idx);
            return AVERROR(ENOMEM);
        }

        uint8_t *d = pkt->data;
        d[0] = 0x80;
        d[1] = s->payload_type & 0x7F;
        uint16_t seq_net = htons(s->seq++);
        memcpy(d + 2, &seq_net, 2);
        uint32_t cur_ts = htonl(base_ts + sample_offset);
        memcpy(d + 4, &cur_ts, 4);
        uint32_t ssrc_net = htonl(s->ssrc);
        memcpy(d + 8, &ssrc_net, 4);

        memcpy(d + 12, src + (sample_offset * frame_stride), payload_len);

        pkt->pts = frame->pts;
        pkt->time_base = (AVRational){ 1, s->sample_rate };

        /* Propagate PTP SideData */
        for (int si = 0; si < frame->nb_side_data; si++) {
            const AVFrameSideData *sd = frame->side_data[si];
            uint8_t *dst_sd = av_packet_new_side_data(pkt, sd->type, sd->size);
            if (dst_sd) memcpy(dst_sd, sd->data, sd->size);
        }

        pkts[pkt_idx++] = pkt;
        sample_offset += n;
        remaining -= n;
    }

    *out_pkts = pkts;
    *nb_out_pkts = pkt_idx;
    return 0;
}

void zstr_st2110_30_payloader_free(zstr_st2110_30_payloader_t **ps)
{
    if (!ps || !*ps) return;
    free(*ps);
    *ps = NULL;
}

/* ---------------------------------------------------------------------------
 * ST 2110-30 Depayloader
 * --------------------------------------------------------------------------- */
struct zstr_st2110_30_depayloader {
    int channels;
    int sample_rate;
    int bit_depth;
    uint8_t *pcm_buf;
    int pcm_len;
    int pcm_cap;
    int64_t pts;
};

zstr_st2110_30_depayloader_t* zstr_st2110_30_depayloader_create(const zstr_st2110_30_config_t *cfg)
{
    zstr_st2110_30_depayloader_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->channels = (cfg && cfg->channels > 0) ? cfg->channels : 2;
    s->sample_rate = (cfg && cfg->sample_rate > 0) ? cfg->sample_rate : 48000;
    s->bit_depth = (cfg && cfg->bit_depth > 0) ? cfg->bit_depth : 16;
    s->pcm_cap = 65536;
    s->pcm_buf = malloc(s->pcm_cap);

    return s;
}

int zstr_st2110_30_depayloader_process(zstr_st2110_30_depayloader_t *s,
                                       const AVPacket *rtp_pkt,
                                       AVFrame *out_frame,
                                       bool *ready)
{
    if (!s || !rtp_pkt || !out_frame || !ready) return AVERROR(EINVAL);
    *ready = false;

    if (rtp_pkt->size <= 12) return AVERROR_INVALIDDATA;

    const uint8_t *payload = rtp_pkt->data + 12;
    int payload_len = rtp_pkt->size - 12;

    int bytes_per_sample = s->bit_depth / 8;
    int frame_stride = s->channels * bytes_per_sample;
    int incoming_samples = payload_len / frame_stride;

    if (s->pcm_len == 0) {
        s->pts = rtp_pkt->pts;
    }

    if (s->pcm_len + payload_len > s->pcm_cap) {
        s->pcm_cap = (s->pcm_len + payload_len) * 2;
        s->pcm_buf = realloc(s->pcm_buf, s->pcm_cap);
    }

    memcpy(s->pcm_buf + s->pcm_len, payload, payload_len);
    s->pcm_len += payload_len;

    /* Emit frame on each packet (or once full) */
    out_frame->nb_samples = s->pcm_len / frame_stride;
    out_frame->sample_rate = s->sample_rate;
    out_frame->format = (s->bit_depth == 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
    av_channel_layout_default(&out_frame->ch_layout, s->channels);

    if (av_frame_get_buffer(out_frame, 0) < 0) return AVERROR(ENOMEM);
    memcpy(out_frame->data[0], s->pcm_buf, s->pcm_len);
    out_frame->pts = s->pts;
    out_frame->time_base = (AVRational){ 1, s->sample_rate };

    /* Propagate side data */
    for (int si = 0; si < rtp_pkt->side_data_elems; si++) {
        const AVPacketSideData *sd = &rtp_pkt->side_data[si];
        AVFrameSideData *fsd = av_frame_new_side_data(out_frame, sd->type, sd->size);
        if (fsd) memcpy(fsd->data, sd->data, sd->size);
    }

    s->pcm_len = 0;
    *ready = true;
    return 0;
}

void zstr_st2110_30_depayloader_free(zstr_st2110_30_depayloader_t **ps)
{
    if (!ps || !*ps) return;
    zstr_st2110_30_depayloader_t *s = *ps;
    free(s->pcm_buf);
    free(s);
    *ps = NULL;
}
