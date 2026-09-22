/*=============================================================================
    test_st2110.c — Unit tests for SMPTE ST 2110 Broadcast Suite & ST 2022-7
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>

#include "zff/plugins/zstr_st2110.h"
#include "zff/plugins/zstr_st2110_toolkit.h"
#include "zff/plugins/zstr_st2110_sdp.h"
#include "zff/zff_core.h"

static void test_st2110_20_video_roundtrip(void)
{
    printf("[TEST] Testing ST 2110-20 Video (RFC 4175) Payloader & Depayloader...\n");

    int w = 320;
    int h = 240;
    zstr_st2110_20_config_t cfg = {
        .width = w,
        .height = h,
        .fmt = AV_PIX_FMT_UYVY422,
        .payload_type = 96,
        .ssrc = 0x21102001,
        .max_payload_bytes = 1200
    };

    zstr_st2110_20_payloader_t *pay = zstr_st2110_20_payloader_create(&cfg);
    zstr_st2110_20_depayloader_t *depay = zstr_st2110_20_depayloader_create(&cfg);
    assert(pay != NULL && depay != NULL);

    /* Create test frame with distinct pattern */
    AVFrame *in_frame = av_frame_alloc();
    in_frame->width = w;
    in_frame->height = h;
    in_frame->format = AV_PIX_FMT_UYVY422;
    int ret = av_frame_get_buffer(in_frame, 64);
    assert(ret >= 0);

    for (int y = 0; y < h; y++) {
        uint8_t *row = in_frame->data[0] + y * in_frame->linesize[0];
        for (int x = 0; x < w * 2; x++) {
            row[x] = (uint8_t)((x + y * 7) & 0xFF);
        }
    }
    in_frame->pts = 90000;

    /* Add PTP SideData */
    uint64_t ptp_ns = 1718000000123456789ULL;
    AVFrameSideData *sd = av_frame_new_side_data(in_frame, ZSTR_TAG_PTP, sizeof(ptp_ns));
    assert(sd != NULL);
    memcpy(sd->data, &ptp_ns, sizeof(ptp_ns));

    /* Packetize */
    AVPacket **pkts = NULL;
    int nb_pkts = 0;
    ret = zstr_st2110_20_payloader_process(pay, in_frame, &pkts, &nb_pkts);
    assert(ret == 0 && nb_pkts > 1);

    /* Depacketize */
    AVFrame *out_frame = av_frame_alloc();
    bool ready = false;
    for (int i = 0; i < nb_pkts; i++) {
        ret = zstr_st2110_20_depayloader_process(depay, pkts[i], out_frame, &ready);
        assert(ret == 0);
        if (i < nb_pkts - 1) {
            assert(ready == false);
        } else {
            assert(ready == true);
        }
    }

    assert(out_frame->width == w && out_frame->height == h);
    assert(out_frame->format == AV_PIX_FMT_UYVY422);

    /* Verify pixels match */
    for (int y = 0; y < h; y++) {
        const uint8_t *src = in_frame->data[0] + y * in_frame->linesize[0];
        const uint8_t *dst = out_frame->data[0] + y * out_frame->linesize[0];
        assert(memcmp(src, dst, w * 2) == 0);
    }

    /* Verify PTP SideData */
    AVFrameSideData *rx_sd = av_frame_get_side_data(out_frame, ZSTR_TAG_PTP);
    assert(rx_sd != NULL && rx_sd->size == sizeof(uint64_t));
    uint64_t rx_ptp = 0;
    memcpy(&rx_ptp, rx_sd->data, sizeof(rx_ptp));
    assert(rx_ptp == ptp_ns);

    zstr_st2110_20_payloader_free_packets(pkts, nb_pkts);
    av_frame_free(&in_frame);
    av_frame_free(&out_frame);
    zstr_st2110_20_payloader_free(&pay);
    zstr_st2110_20_depayloader_free(&depay);

    printf("[PASS] ST 2110-20 Video roundtrip passed.\n");
}

static void test_st2110_30_audio_roundtrip(void)
{
    printf("[TEST] Testing ST 2110-30 Audio (AES67 PCM) Payloader & Depayloader...\n");

    zstr_st2110_30_config_t cfg = {
        .channels = 2,
        .sample_rate = 48000,
        .bit_depth = 16,
        .payload_type = 97,
        .ssrc = 0x21103001,
        .packet_time_us = 1000 /* 1ms = 48 samples */
    };

    zstr_st2110_30_payloader_t *pay = zstr_st2110_30_payloader_create(&cfg);
    zstr_st2110_30_depayloader_t *depay = zstr_st2110_30_depayloader_create(&cfg);
    assert(pay != NULL && depay != NULL);

    int nb_samples = 480; /* 10ms */
    AVFrame *in_audio = av_frame_alloc();
    in_audio->nb_samples = nb_samples;
    in_audio->sample_rate = 48000;
    in_audio->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&in_audio->ch_layout, 2);
    int ret = av_frame_get_buffer(in_audio, 0);
    assert(ret >= 0);

    int16_t *samples = (int16_t*)in_audio->data[0];
    for (int i = 0; i < nb_samples * 2; i++) {
        samples[i] = (int16_t)(i * 100);
    }
    in_audio->pts = 48000;

    /* Packetize */
    AVPacket **pkts = NULL;
    int nb_pkts = 0;
    ret = zstr_st2110_30_payloader_process(pay, in_audio, &pkts, &nb_pkts);
    assert(ret == 0);
    assert(nb_pkts == 10); /* 480 / 48 = 10 packets */

    /* Depacketize first packet (48 samples) */
    AVFrame *out_audio = av_frame_alloc();
    bool ready = false;
    ret = zstr_st2110_30_depayloader_process(depay, pkts[0], out_audio, &ready);
    assert(ret == 0 && ready == true);
    assert(out_audio->nb_samples == 48);
    assert(memcmp(out_audio->data[0], in_audio->data[0], 48 * 4) == 0);

    zstr_st2110_20_payloader_free_packets(pkts, nb_pkts);
    av_frame_free(&in_audio);
    av_frame_free(&out_audio);
    zstr_st2110_30_payloader_free(&pay);
    zstr_st2110_30_depayloader_free(&depay);

    printf("[PASS] ST 2110-30 Audio roundtrip passed.\n");
}

static void test_st2022_7_redundancy(void)
{
    printf("[TEST] Testing SMPTE ST 2022-7 Hitless Redundancy deduplication...\n");

    zstr_st2022_7_demux_t *demux = zstr_st2022_7_demux_create();
    assert(demux != NULL);

    /* Simulate dual path A/B receiving duplicate packets */
    for (uint16_t seq = 100; seq < 120; seq++) {
        AVPacket *pkt_a = av_packet_alloc();
        av_new_packet(pkt_a, 20);
        pkt_a->data[0] = 0x80;
        pkt_a->data[2] = (uint8_t)(seq >> 8);
        pkt_a->data[3] = (uint8_t)(seq & 0xFF);

        AVPacket *pkt_b = av_packet_alloc();
        av_new_packet(pkt_b, 20);
        memcpy(pkt_b->data, pkt_a->data, 20);

        bool is_dup_a = false;
        int ret = zstr_st2022_7_demux_process(demux, 0, pkt_a, &is_dup_a);
        assert(ret == 0);
        assert(is_dup_a == false); /* Path A is new */

        bool is_dup_b = false;
        ret = zstr_st2022_7_demux_process(demux, 1, pkt_b, &is_dup_b);
        assert(ret == 0);
        assert(is_dup_b == true);  /* Path B duplicate successfully rejected */

        av_packet_free(&pkt_a);
        av_packet_free(&pkt_b);
    }

    /* Simulate Path A failure: packets 120..130 only arrive on Path B */
    for (uint16_t seq = 120; seq < 130; seq++) {
        AVPacket *pkt_b = av_packet_alloc();
        av_new_packet(pkt_b, 20);
        pkt_b->data[0] = 0x80;
        pkt_b->data[2] = (uint8_t)(seq >> 8);
        pkt_b->data[3] = (uint8_t)(seq & 0xFF);

        bool is_dup_b = true;
        int ret = zstr_st2022_7_demux_process(demux, 1, pkt_b, &is_dup_b);
        assert(ret == 0);
        assert(is_dup_b == false); /* Path B packets accepted seamlessly during Path A outage */
        av_packet_free(&pkt_b);
    }

    zstr_st2022_7_demux_free(&demux);
    printf("[PASS] SMPTE ST 2022-7 Hitless Redundancy passed.\n");
}

static void test_st2110_40_anc_roundtrip(void)
{
    printf("[TEST] Testing ST 2110-40 ANC payloader & depayloader...\n");

    zstr_st2110_40_payloader_t *pay =
        zstr_st2110_40_payloader_create(&(zstr_st2110_40_config_t){
            .payload_type = 100, .ssrc = 0x21104001, .mtu = 1400 });
    zstr_st2110_40_depayloader_t *depay =
        zstr_st2110_40_depayloader_create(&(zstr_st2110_40_config_t){
            .payload_type = 100 });
    assert(pay != NULL && depay != NULL);

    /* Build two ANC packets: CEA-608 (DID 0x61) + OP-47 (DID 0x43) */
    uint8_t anc[64];
    int pos = 0;
    /* pkt1: DID=0x61 SDID=0x01 DC=3 UDW + CS */
    anc[pos++] = 0x61; anc[pos++] = 0x01; anc[pos++] = 3;
    anc[pos++] = 0x10; anc[pos++] = 0x20; anc[pos++] = 0x30;
    anc[pos++] = (uint8_t)((0x61 + 0x01 + 3 + 0x10 + 0x20 + 0x30) & 0xFF);
    /* pkt2: DID=0x43 SDID=0x02 DC=2 UDW + CS */
    anc[pos++] = 0x43; anc[pos++] = 0x02; anc[pos++] = 2;
    anc[pos++] = 0xAA; anc[pos++] = 0xBB;
    anc[pos++] = (uint8_t)((0x43 + 0x02 + 2 + 0xAA + 0xBB) & 0xFF);
    int anc_len = pos;

    AVPacket *in_pkt = av_packet_alloc();
    av_new_packet(in_pkt, anc_len);
    memcpy(in_pkt->data, anc, anc_len);
    in_pkt->pts = 180000;
    in_pkt->time_base = (AVRational){ 1, 90000 };

    AVPacket **rtp_pkts = NULL;
    int nb_pkts = 0;
    int ret = zstr_st2110_40_payloader_process(pay, in_pkt, &rtp_pkts, &nb_pkts);
    assert(ret == 0);
    assert(nb_pkts == 1); /* fits in one MTU */
    assert(rtp_pkts[0]->size == 12 + anc_len);
    assert((rtp_pkts[0]->data[1] & 0x7F) == 100);
    assert((rtp_pkts[0]->data[1] & 0x80) != 0); /* marker on last */
    uint32_t ts = ((uint32_t)rtp_pkts[0]->data[4] << 24) |
                  ((uint32_t)rtp_pkts[0]->data[5] << 16) |
                  ((uint32_t)rtp_pkts[0]->data[6] << 8) |
                  rtp_pkts[0]->data[7];
    assert(ts == 180000);

    AVPacket *out_pkt = av_packet_alloc();
    bool ready = false;
    ret = zstr_st2110_40_depayloader_process(depay, rtp_pkts[0], out_pkt, &ready);
    assert(ret == 0);
    assert(ready == true);
    assert(out_pkt->size == anc_len);
    assert(memcmp(out_pkt->data, anc, anc_len) == 0);

    zstr_st2110_40_payloader_free_packets(rtp_pkts, nb_pkts);
    av_packet_free(&in_pkt);
    av_packet_free(&out_pkt);

    /* Multi-packet fragmentation with tiny MTU + corrupt tail rejection */
    zstr_st2110_40_payloader_t *pay2 =
        zstr_st2110_40_payloader_create(&(zstr_st2110_40_config_t){
            .payload_type = 100, .ssrc = 0x21104002, .mtu = 64 });
    uint8_t big[200];
    for (int i = 0; i < 200; i++) big[i] = (uint8_t)i;
    /* Make it valid ANC framing: chain of DC=... use DID=0x61 DC=60 chunks */
    int bp = 0;
    while (bp + 64 <= 200) {
        big[bp++] = 0x61; big[bp++] = 0x01; big[bp++] = 60;
        bp += 60;
        big[bp++] = 0x00; /* CS placeholder */
    }
    AVPacket *big_pkt = av_packet_alloc();
    av_new_packet(big_pkt, bp);
    memcpy(big_pkt->data, big, bp);
    big_pkt->pts = 270000;
    big_pkt->time_base = (AVRational){ 1, 90000 };

    rtp_pkts = NULL;
    nb_pkts = 0;
    ret = zstr_st2110_40_payloader_process(pay2, big_pkt, &rtp_pkts, &nb_pkts);
    assert(ret == 0);
    assert(nb_pkts > 1);
    for (int i = 0; i < nb_pkts; i++) {
        bool m = (rtp_pkts[i]->data[1] & 0x80) != 0;
        assert(m == (i == nb_pkts - 1)); /* marker only on last */
    }

    out_pkt = av_packet_alloc();
    ready = false;
    for (int i = 0; i < nb_pkts; i++) {
        ret = zstr_st2110_40_depayloader_process(depay, rtp_pkts[i], out_pkt, &ready);
        assert(ret == 0);
        assert(ready == (i == nb_pkts - 1));
    }
    assert(ready == true);
    assert(out_pkt->size == bp);
    assert(memcmp(out_pkt->data, big, bp) == 0);

    /* Corrupt: truncated final packet must be rejected */
    AVPacket *bad = av_packet_alloc();
    av_new_packet(bad, 12 + 3);
    memcpy(bad->data, rtp_pkts[nb_pkts - 1]->data, 12);
    bad->data[1] |= 0x80;
    bad->data[12] = 0x61; bad->data[13] = 0x01; bad->data[14] = 60; /* claims 60, has 0 */
    ret = zstr_st2110_40_depayloader_process(depay, bad, out_pkt, &ready);
    assert(ret < 0);
    assert(ready == false);

    zstr_st2110_40_payloader_free_packets(rtp_pkts, nb_pkts);
    av_packet_free(&big_pkt);
    av_packet_free(&out_pkt);
    av_packet_free(&bad);
    zstr_st2110_40_payloader_free(&pay);
    zstr_st2110_40_payloader_free(&pay2);
    zstr_st2110_40_depayloader_free(&depay);

    printf("[PASS] ST 2110-40 ANC roundtrip passed.\n");
}

static void test_st2110_21_narrow_pacer(void)
{
    printf("[TEST] Testing ST 2110-21 Narrow sender pacer...\n");

    /* 10 packets at 60 fps: period ~16.7ms, interval ~1.67ms (CI-safe) */
    zstr_st2110_21_pacer_t *pacer =
        zstr_st2110_21_pacer_create(&(zstr_st2110_21_config_t){
            .width = 320, .height = 240, .fps_num = 60, .fps_den = 1,
            .pacer_type = 0 });
    assert(pacer != NULL);

    assert(zstr_st2110_21_frame_start(pacer, 90000, 10) == 0);
    int64_t interval = zstr_st2110_21_packet_interval_ns(pacer);
    assert(interval > 1000000 && interval < 2000000);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int64_t prev = 0;
    int64_t max_gap = 0;
    for (int i = 0; i < 10; i++) {
        assert(zstr_st2110_21_wait_packet(pacer) == 0);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t el = (now.tv_sec - t0.tv_sec) * 1000000000LL +
                     (now.tv_nsec - t0.tv_nsec);
        if (i > 0) {
            int64_t gap = el - prev;
            if (gap > max_gap) max_gap = gap;
        }
        prev = el;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t total = (t1.tv_sec - t0.tv_sec) * 1000000000LL +
                    (t1.tv_nsec - t0.tv_nsec);
    /* Total spans ~9 intervals (~15ms): generous CI bounds */
    assert(total >= 5000000LL);
    assert(total <= 60000000LL);
    /* No bursts: max inter-packet gap bounded (generous for CI) */
    assert(max_gap <= 10000000LL);
    assert(zstr_st2110_21_late_count(pacer) == 0);

    /* 11th wait on a 10-packet frame must fail */
    assert(zstr_st2110_21_wait_packet(pacer) < 0);

    /* Same timestamp re-announce is idempotent, new timestamp restarts */
    assert(zstr_st2110_21_frame_start(pacer, 90000, 10) == 0);
    assert(zstr_st2110_21_frame_start(pacer, 91800, 10) == 0);
    assert(zstr_st2110_21_wait_packet(pacer) == 0);

    /* Forced lateness: sleep past the whole frame, next wait must return
     * immediately (no catch-up burst) and count one late slot. */
    assert(zstr_st2110_21_frame_start(pacer, 93600, 10) == 0);
    struct timespec sl = { 0, 100 * 1000 * 1000 };
    nanosleep(&sl, NULL);
    struct timespec b0, b1;
    clock_gettime(CLOCK_MONOTONIC, &b0);
    assert(zstr_st2110_21_wait_packet(pacer) == 0);
    clock_gettime(CLOCK_MONOTONIC, &b1);
    int64_t dt = (b1.tv_sec - b0.tv_sec) * 1000000000LL +
                 (b1.tv_nsec - b0.tv_nsec);
    assert(dt < 20000000LL); /* returned immediately, did not sleep a slot */
    assert(zstr_st2110_21_late_count(pacer) == 1);

    zstr_st2110_21_pacer_free(&pacer);

    printf("[PASS] ST 2110-21 Narrow pacer passed.\n");
}

static void test_st2022_5_row_fec(void)
{
    printf("[TEST] Testing ST 2022-5 row FEC encode/recover...\n");

    zstr_st2022_5_fec_t *enc =
        zstr_st2022_5_fec_create(&(zstr_st2022_5_config_t){ .row_len = 4, .fec_pt = 127 });
    zstr_st2022_5_fec_decoder_t *dec =
        zstr_st2022_5_fec_decoder_create(&(zstr_st2022_5_config_t){ .row_len = 4, .fec_pt = 127 },
                                        96);
    assert(enc != NULL && dec != NULL);

    /* Build one row of 4 media packets (PT=96, varying payload) */
    AVPacket *media[4];
    for (int i = 0; i < 4; i++) {
        media[i] = av_packet_alloc();
        av_new_packet(media[i], 12 + 100 + i * 10);
        uint8_t *d = media[i]->data;
        d[0] = 0x80;
        d[1] = 96 | (i == 3 ? 0x80 : 0x00);
        d[2] = 3; d[3] = (uint8_t)(0xE8 + i); /* seq 1000..1003 */
        uint32_t ts = 90000;
        d[4] = (ts >> 24) & 0xFF; d[5] = (ts >> 16) & 0xFF;
        d[6] = (ts >> 8) & 0xFF; d[7] = ts & 0xFF;
        d[8] = 0x21; d[9] = 0x10; d[10] = 0x20; d[11] = 0x01;
        for (int k = 0; k < 100 + i * 10; k++) d[12 + k] = (uint8_t)(i * 17 + k);
        media[i]->pts = 90000;
        media[i]->time_base = (AVRational){ 1, 90000 };
    }

    AVPacket *fec = NULL;
    for (int i = 0; i < 4; i++) {
        AVPacket *f = zstr_st2022_5_fec_encode(enc, media[i]);
        if (i < 3) assert(f == NULL);
        else { assert(f != NULL); fec = f; }
    }
    assert(fec != NULL);
    assert((fec->data[1] & 0x7F) == 127);
    /* SN base + length recovery + PT recovery + mask + TS recovery */
    assert(fec->data[12] == 3 && fec->data[13] == 0xE8); /* base 1000 */
    assert(fec->data[16] == 96);
    assert(fec->size == 24 + 130); /* max payload 130 */

    /* Drop packet 2 (seq 1002), feed the rest + FEC */
    for (int i = 0; i < 4; i++) {
        if (i == 2) continue;
        assert(zstr_st2022_5_fec_decoder_media(dec, media[i]) == 0);
    }
    AVPacket *rec = zstr_st2022_5_fec_decoder_fec(dec, fec);
    assert(rec != NULL);
    assert(rec->size == media[2]->size);
    assert(memcmp(rec->data, media[2]->data, media[2]->size) == 0);
    assert(zstr_st2022_5_fec_recovered(dec) == 1);

    /* Drop two of four: unrecoverable (fresh row at seq 2000) */
    zstr_st2022_5_fec_t *enc2 =
        zstr_st2022_5_fec_create(&(zstr_st2022_5_config_t){ .row_len = 4, .fec_pt = 127 });
    zstr_st2022_5_fec_decoder_t *dec2 =
        zstr_st2022_5_fec_decoder_create(&(zstr_st2022_5_config_t){ .row_len = 4, .fec_pt = 127 },
                                        96);
    AVPacket *m2[4], *fec2 = NULL;
    for (int i = 0; i < 4; i++) {
        m2[i] = av_packet_alloc();
        av_new_packet(m2[i], 12 + 80);
        uint8_t *d = m2[i]->data;
        d[0] = 0x80; d[1] = 96;
        d[2] = 0; d[3] = (uint8_t)(2000 + i);
        memset(d + 4, 0, 8);
        for (int k = 0; k < 80; k++) d[12 + k] = (uint8_t)(i + k);
        AVPacket *f = zstr_st2022_5_fec_encode(enc2, m2[i]);
        if (f) fec2 = f;
    }
    assert(fec2 != NULL);
    assert(zstr_st2022_5_fec_decoder_media(dec2, m2[0]) == 0);
    assert(zstr_st2022_5_fec_decoder_media(dec2, m2[3]) == 0);
    AVPacket *rec2 = zstr_st2022_5_fec_decoder_fec(dec2, fec2);
    assert(rec2 == NULL);
    assert(zstr_st2022_5_fec_unrecoverable(dec2) == 1);
    for (int i = 0; i < 4; i++) av_packet_free(&m2[i]);
    av_packet_free(&fec2);
    zstr_st2022_5_fec_free(&enc2);
    zstr_st2022_5_fec_decoder_free(&dec2);

    /* Wrap-around: row spanning 65534..65535,0,1 recovers the middle loss */
    zstr_st2022_5_fec_t *enc3 =
        zstr_st2022_5_fec_create(&(zstr_st2022_5_config_t){ .row_len = 4, .fec_pt = 127 });
    zstr_st2022_5_fec_decoder_t *dec3 =
        zstr_st2022_5_fec_decoder_create(&(zstr_st2022_5_config_t){ .row_len = 4, .fec_pt = 127 },
                                        96);
    AVPacket *fec3 = NULL;
    AVPacket *wm[4];
    for (int i = 0; i < 4; i++) {
        uint16_t sq = (uint16_t)(65534 + i);
        wm[i] = av_packet_alloc();
        av_new_packet(wm[i], 12 + 64);
        uint8_t *d = wm[i]->data;
        d[0] = 0x80; d[1] = 96;
        d[2] = (uint8_t)(sq >> 8); d[3] = (uint8_t)(sq & 0xFF);
        memset(d + 4, 0, 8);
        for (int k = 0; k < 64; k++) d[12 + k] = (uint8_t)(i + k);
        AVPacket *f = zstr_st2022_5_fec_encode(enc3, wm[i]);
        if (f) fec3 = f;
    }
    assert(fec3 != NULL);
    for (int i = 0; i < 4; i++) {
        if (i == 2) continue; /* lose seq 0 */
        assert(zstr_st2022_5_fec_decoder_media(dec3, wm[i]) == 0);
    }
    AVPacket *rec3 = zstr_st2022_5_fec_decoder_fec(dec3, fec3);
    assert(rec3 != NULL);
    assert(rec3->size == wm[2]->size);
    assert(memcmp(rec3->data, wm[2]->data, wm[2]->size) == 0);

    for (int i = 0; i < 4; i++) {
        av_packet_free(&media[i]);
        av_packet_free(&wm[i]);
    }
    av_packet_free(&fec);
    av_packet_free(&rec);
    av_packet_free(&rec3);
    av_packet_free(&fec3);
    zstr_st2022_5_fec_free(&enc);
    zstr_st2022_5_fec_free(&enc3);
    zstr_st2022_5_fec_decoder_free(&dec);
    zstr_st2022_5_fec_decoder_free(&dec3);

    printf("[PASS] ST 2022-5 row FEC passed.\n");
}

static void test_st2110_22_rfc9134_roundtrip(void)
{
    printf("[TEST] Testing ST 2110-22 RFC 9134 codestream packetization...\n");

    zstr_st2110_22_payloader_t *pay =
        zstr_st2110_22_payloader_create(&(zstr_st2110_22_config_t){
            .payload_type = 96, .ssrc = 0x21102201, .mtu = 256 });
    zstr_st2110_22_depayloader_t *depay =
        zstr_st2110_22_depayloader_create(&(zstr_st2110_22_config_t){
            .payload_type = 96 });
    assert(pay != NULL && depay != NULL);

    /* Synthetic codestream: SOC + header + slices + EOC */
    uint8_t cs[1200];
    int pos = 0;
    cs[pos++] = 0xFF; cs[pos++] = 0x10; /* SOC */
    for (int i = 0; i < 100; i++) cs[pos++] = (uint8_t)(0x20 + i);
    for (int sli = 0; sli < 3; sli++) {
        cs[pos++] = 0xFF; cs[pos++] = 0x22; /* SLH-ish */
        for (int i = 0; i < 300; i++) cs[pos++] = (uint8_t)(sli * 40 + i);
    }
    cs[pos++] = 0xFF; cs[pos++] = 0x11; /* EOC */
    int cs_len = pos;

    AVPacket *in_pkt = av_packet_alloc();
    av_new_packet(in_pkt, cs_len);
    memcpy(in_pkt->data, cs, cs_len);
    in_pkt->pts = 180000;
    in_pkt->time_base = (AVRational){ 1, 90000 };

    AVPacket **rtp_pkts = NULL;
    int nb_pkts = 0;
    int ret = zstr_st2110_22_payloader_process(pay, in_pkt, &rtp_pkts, &nb_pkts);
    assert(ret == 0);
    assert(nb_pkts > 1); /* 1206 bytes over 240-byte payloads */
    for (int i = 0; i < nb_pkts; i++) {
        uint8_t *d = rtp_pkts[i]->data;
        assert((d[0] >> 6) == 2);
        bool m = (d[1] & 0x80) != 0;
        assert(m == (i == nb_pkts - 1));
        /* Payload header: T=1 K=0 L=M I=00 */
        assert((d[12] & 0x80) != 0); /* T */
        assert((d[12] & 0x40) == 0); /* K */
        assert(((d[12] & 0x20) != 0) == m); /* L == M */
        assert((d[12] & 0x18) == 0); /* I */
        assert((d[12] & 0x1F) == 0); /* F counter 0 (first frame) */
        /* P counter increments */
        int p = ((d[14] & 0x1F) << 8) | d[15];
        assert(p == i);
    }

    AVPacket *out_pkt = av_packet_alloc();
    bool ready = false;
    for (int i = 0; i < nb_pkts; i++) {
        ret = zstr_st2110_22_depayloader_process(depay, rtp_pkts[i], out_pkt, &ready);
        assert(ret == 0);
        assert(ready == (i == nb_pkts - 1));
    }
    assert(ready == true);
    assert(out_pkt->size == cs_len);
    assert(memcmp(out_pkt->data, cs, cs_len) == 0);

    zstr_st2110_22_payloader_free_packets(rtp_pkts, nb_pkts);
    av_packet_free(&in_pkt);
    av_packet_free(&out_pkt);
    zstr_st2110_22_payloader_free(&pay);
    zstr_st2110_22_depayloader_free(&depay);

    printf("[PASS] ST 2110-22 RFC 9134 roundtrip passed.\n");
}

static void test_st2110_22_encode_decode(void)
{
    printf("[TEST] Testing ST 2110-22 SVT-JPEG-XS encode/decode...\n");

    zstr_st2110_22_encoder_t *enc =
        zstr_st2110_22_encoder_create(&(zstr_st2110_22_config_t){
            .width = 640, .height = 480, .bpp_num = 3, .bpp_den = 1 });
    if (!enc) {
        printf("[SKIP] SVT-JPEG-XS not available, skipping encode/decode.\n");
        return;
    }
    zstr_st2110_22_decoder_t *dec =
        zstr_st2110_22_decoder_create(&(zstr_st2110_22_config_t){
            .width = 640, .height = 480 });
    assert(dec != NULL);

    AVFrame *in = av_frame_alloc();
    in->width = 640;
    in->height = 480;
    in->format = AV_PIX_FMT_YUV422P;
    assert(av_frame_get_buffer(in, 32) == 0);
    assert(av_frame_make_writable(in) == 0);
    for (int y = 0; y < 480; y++) {
        memset(in->data[0] + y * in->linesize[0], (y * 255) / 480, 640);
        memset(in->data[1] + y * in->linesize[1], 128, 320);
        memset(in->data[2] + y * in->linesize[2], 128, 320);
    }
    in->pts = 0;

    AVPacket *coded = NULL;
    int ret = zstr_st2110_22_encode(enc, in, &coded);
    assert(ret == 0 && coded != NULL);
    assert(coded->size > 100); /* real codestream, not empty */
    assert(coded->data[0] == 0xFF && coded->data[1] == 0x10); /* SOC */
    printf("[INFO] JPEG XS codestream: %d bytes (%.2f bpp).\n",
           coded->size, coded->size * 8.0 / (640 * 480));

    AVFrame *out = NULL;
    ret = zstr_st2110_22_decode(dec, coded, &out);
    assert(ret == 0 && out != NULL);
    assert(out->width == 640 && out->height == 480);
    assert(out->format == AV_PIX_FMT_YUV422P);

    /* Sanity: vertical gradient survives (top dark, bottom bright) */
    int top = out->data[0][10 * out->linesize[0] + 320];
    int bottom = out->data[0][470 * out->linesize[0] + 320];
    assert(bottom > top + 50);

    av_frame_free(&in);
    av_frame_free(&out);
    av_packet_free(&coded);
    zstr_st2110_22_encoder_free(&enc);
    zstr_st2110_22_decoder_free(&dec);

    printf("[PASS] ST 2110-22 encode/decode passed.\n");
}

static void test_st2022_7_mux_dualsend(void)
{
    printf("[TEST] Testing SMPTE ST 2022-7 dual-send mux + demux loop...\n");

    zstr_st2022_7_mux_t *mux = zstr_st2022_7_mux_create();
    zstr_st2022_7_demux_t *demux = zstr_st2022_7_demux_create();
    assert(mux != NULL && demux != NULL);

    int forwarded = 0;
    for (uint16_t seq = 200; seq < 210; seq++) {
        AVPacket *in = av_packet_alloc();
        av_new_packet(in, 20);
        in->data[0] = 0x80;
        in->data[1] = 96;
        in->data[2] = (uint8_t)(seq >> 8);
        in->data[3] = (uint8_t)(seq & 0xFF);

        AVPacket *a = NULL, *b = NULL;
        int ret = zstr_st2022_7_mux_process(mux, in, &a, &b);
        assert(ret == 0);
        assert(a != NULL && b != NULL);
        /* Bit-identical copies per ST 2022-7 */
        assert(a->size == b->size && a->size == in->size);
        assert(memcmp(a->data, b->data, a->size) == 0);

        bool dup_a = true, dup_b = true;
        assert(zstr_st2022_7_demux_process(demux, 0, a, &dup_a) == 0);
        assert(dup_a == false);
        forwarded++;
        assert(zstr_st2022_7_demux_process(demux, 1, b, &dup_b) == 0);
        assert(dup_b == true); /* late duplicate dropped */

        av_packet_free(&in);
        av_packet_free(&a);
        av_packet_free(&b);
    }
    assert(forwarded == 10);
    assert(zstr_st2022_7_mux_count(mux) == 10);

    /* Path A outage: B-only arrivals still forward (hitless) */
    for (uint16_t seq = 210; seq < 215; seq++) {
        AVPacket *in = av_packet_alloc();
        av_new_packet(in, 20);
        in->data[0] = 0x80;
        in->data[2] = (uint8_t)(seq >> 8);
        in->data[3] = (uint8_t)(seq & 0xFF);
        AVPacket *a = NULL, *b = NULL;
        assert(zstr_st2022_7_mux_process(mux, in, &a, &b) == 0);
        av_packet_free(&a); /* path A lost */
        bool dup = true;
        assert(zstr_st2022_7_demux_process(demux, 1, b, &dup) == 0);
        assert(dup == false);
        forwarded++;
        av_packet_free(&in);
        av_packet_free(&b);
    }
    assert(forwarded == 15);

    zstr_st2022_7_mux_free(&mux);
    zstr_st2022_7_demux_free(&demux);
    printf("[PASS] ST 2022-7 dual-send mux passed.\n");
}

static void test_st2110_sdp_roundtrip(void)
{
    printf("[TEST] Testing ST 2110 SDP generate + parse...\n");

    zstr_st2110_sdp_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.address, sizeof(cfg.address), "239.10.10.1");
    snprintf(cfg.session_name, sizeof(cfg.session_name), "zff-test");
    snprintf(cfg.ptp_address, sizeof(cfg.ptp_address), "192.168.10.1");
    cfg.ptp_domain = 127;
    cfg.video_enabled = 1;
    cfg.video_port = 20000;
    cfg.video_pt = 96;
    snprintf(cfg.video_sampling, sizeof(cfg.video_sampling), "YCbCr-4:2:2");
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.depth = 10;
    cfg.audio_enabled = 1;
    cfg.audio_port = 20002;
    cfg.audio_pt = 97;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.audio_depth = 24;

    char sdp[4096];
    int n = zstr_st2110_sdp_generate(&cfg, sdp, sizeof(sdp));
    assert(n > 0);
    assert(strstr(sdp, "m=video 20000 RTP/AVP 96") != NULL);
    assert(strstr(sdp, "a=rtpmap:96 raw/90000") != NULL);
    assert(strstr(sdp, "a=fmtp:96 sampling=YCbCr-4:2:2;width=1920;height=1080;depth=10") != NULL);
    assert(strstr(sdp, "m=audio 20002 RTP/AVP 97") != NULL);
    assert(strstr(sdp, "a=rtpmap:97 L24/48000/2") != NULL);
    assert(strstr(sdp, "a=mediaclk:direct=0") != NULL);
    assert(strstr(sdp, "a=ts-refclk:ptp=IEEE1588-2019:192.168.10.1:127") != NULL);
    assert(strstr(sdp, "c=IN IP4 239.10.10.1") != NULL);

    /* Parse our own output back */
    zstr_st2110_sdp_config_t back;
    memset(&back, 0, sizeof(back));
    assert(zstr_st2110_sdp_parse(sdp, &back) == 0);
    assert(back.video_enabled && back.video_port == 20000 && back.video_pt == 96);
    assert(strcmp(back.video_sampling, "YCbCr-4:2:2") == 0);
    assert(back.width == 1920 && back.height == 1080 && back.depth == 10);
    assert(back.audio_enabled && back.audio_port == 20002 && back.audio_pt == 97);
    assert(back.sample_rate == 48000 && back.channels == 2 && back.audio_depth == 24);
    assert(strcmp(back.address, "239.10.10.1") == 0);
    assert(strcmp(back.ptp_address, "192.168.10.1") == 0 && back.ptp_domain == 127);

    /* Parse a third-party-style SDP (different order, extra lines, LF-only) */
    const char *foreign_sdp =
        "v=0\n"
        "o=- 12345 1 IN IP4 10.0.0.5\n"
        "s=Foreign 2110 Sender\n"
        "c=IN IP4 239.20.20.2\n"
        "t=0 0\n"
        "a=tool:vendor-x\n"
        "a=ts-refclk:ptp=IEEE1588-2019:10.0.0.1:0\n"
        "m=audio 30002 RTP/AVP 98\n"
        "a=rtpmap:98 L16/48000/8\n"
        "a=mediaclk:direct=0\n"
        "a=ptime:0.125\n"
        "m=video 30000 RTP/AVP 99\n"
        "a=rtpmap:99 raw/90000\n"
        "a=fmtp:99 sampling=RGB;width=1280;height=720;depth=8;interlace\n";
    memset(&back, 0, sizeof(back));
    assert(zstr_st2110_sdp_parse(foreign_sdp, &back) == 0);
    assert(back.audio_enabled && back.audio_port == 30002 && back.audio_pt == 98);
    assert(back.sample_rate == 48000 && back.channels == 8 && back.audio_depth == 16);
    assert(back.video_enabled && back.video_port == 30000 && back.video_pt == 99);
    assert(strcmp(back.video_sampling, "RGB") == 0);
    assert(back.width == 1280 && back.height == 720 && back.depth == 8);
    assert(strcmp(back.address, "239.20.20.2") == 0);

    /* Garbage in: no media sections -> error */
    memset(&back, 0, sizeof(back));
    assert(zstr_st2110_sdp_parse("v=0\r\ns=empty\r\n", &back) < 0);
    assert(zstr_st2110_sdp_generate(NULL, sdp, sizeof(sdp)) < 0);

    printf("[PASS] ST 2110 SDP roundtrip passed.\n");
}

static void test_st2110_sdp_device_handoff(void)
{
    printf("[TEST] Testing ST 2110 SDP device handoff (mux emits, demux builds)...\n");

    char sdp_path[] = "/tmp/zstr_st2110_sdp_XXXXXX";
    int fd = mkstemp(sdp_path);
    assert(fd >= 0);
    close(fd);

    /* Mux side: video stream + sdp_file; header emits the SDP */
    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_st2110_mux");
    assert(out_fmt != NULL);
    AVFormatContext *out_ctx = NULL;
    assert(avformat_alloc_output_context2(&out_ctx, out_fmt, "zstr_st2110_mux",
                                          "udp://127.0.0.1:25100") >= 0);
    AVStream *vst = avformat_new_stream(out_ctx, NULL);
    assert(vst != NULL);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->width = 1280;
    vst->codecpar->height = 720;
    vst->codecpar->format = AV_PIX_FMT_UYVY422;
    AVDictionary *mux_opts = NULL;
    av_dict_set(&mux_opts, "sdp_file", sdp_path, 0);
    assert(avformat_write_header(out_ctx, &mux_opts) >= 0);
    av_dict_free(&mux_opts);

    /* SDP file describes the sender */
    FILE *f = fopen(sdp_path, "r");
    assert(f != NULL);
    char sdp_text[4096];
    size_t n = fread(sdp_text, 1, sizeof(sdp_text) - 1, f);
    fclose(f);
    sdp_text[n] = '\0';
    assert(strstr(sdp_text, "m=video 25100 RTP/AVP 96") != NULL);
    assert(strstr(sdp_text, "width=1280;height=720;depth=8") != NULL);
    assert(strstr(sdp_text, "c=IN IP4 127.0.0.1") != NULL);

    /* Demux side: streams built from the same SDP file */
    const AVInputFormat *in_fmt = zff_find_input_format("zstr_st2110_demux");
    assert(in_fmt != NULL);
    AVFormatContext *in_ctx = NULL;
    AVDictionary *demux_opts = NULL;
    av_dict_set(&demux_opts, "sdp_file", sdp_path, 0);
    assert(avformat_open_input(&in_ctx, "udp://127.0.0.1:25100", in_fmt, &demux_opts) == 0);
    av_dict_free(&demux_opts);
    assert(in_ctx->nb_streams == 1);
    assert(in_ctx->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
    assert(in_ctx->streams[0]->codecpar->width == 1280);
    assert(in_ctx->streams[0]->codecpar->height == 720);
    assert(in_ctx->streams[0]->codecpar->format == AV_PIX_FMT_UYVY422);

    /* Loopback one packet through the SDP-configured pair */
    AVPacket *tx = av_packet_alloc();
    av_new_packet(tx, 64);
    memset(tx->data, 0x5A, 64);
    tx->stream_index = 0;
    assert(av_write_frame(out_ctx, tx) == 0);
    av_packet_free(&tx);

    AVPacket *rx = av_packet_alloc();
    assert(av_read_frame(in_ctx, rx) == 0);
    assert(rx->size == 64 && rx->data[0] == 0x5A);
    av_packet_free(&rx);

    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);

    /* Audio-only SDP: hand-built dual session, demux builds both streams */
    zstr_st2110_sdp_config_t dual;
    memset(&dual, 0, sizeof(dual));
    snprintf(dual.address, sizeof(dual.address), "127.0.0.1");
    dual.video_enabled = 1;
    dual.video_port = 25200;
    dual.video_pt = 96;
    snprintf(dual.video_sampling, sizeof(dual.video_sampling), "RGB");
    dual.width = 640;
    dual.height = 480;
    dual.depth = 8;
    dual.audio_enabled = 1;
    dual.audio_port = 25202;
    dual.audio_pt = 97;
    dual.sample_rate = 48000;
    dual.channels = 2;
    dual.audio_depth = 16;
    char dual_text[4096];
    assert(zstr_st2110_sdp_generate(&dual, dual_text, sizeof(dual_text)) > 0);
    f = fopen(sdp_path, "w");
    assert(f != NULL);
    fputs(dual_text, f);
    fclose(f);

    in_ctx = NULL;
    demux_opts = NULL;
    av_dict_set(&demux_opts, "sdp_file", sdp_path, 0);
    assert(avformat_open_input(&in_ctx, "udp://127.0.0.1:25200", in_fmt, &demux_opts) == 0);
    av_dict_free(&demux_opts);
    assert(in_ctx->nb_streams == 2);
    assert(in_ctx->streams[0]->codecpar->width == 640);
    assert(in_ctx->streams[0]->codecpar->format == AV_PIX_FMT_RGB24);
    assert(in_ctx->streams[1]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO);
    assert(in_ctx->streams[1]->codecpar->codec_id == AV_CODEC_ID_PCM_S16BE);
    assert(in_ctx->streams[1]->codecpar->sample_rate == 48000);
    avformat_close_input(&in_ctx);

    unlink(sdp_path);
    printf("[PASS] ST 2110 SDP device handoff passed.\n");
}

static void test_st2022_5_column_fec(void)
{
    printf("[TEST] Testing ST 2022-5 column FEC (2-D matrix)...\n");

    /* L=4, D=3: 12 packets, seq 3000..3011, varying lengths */
    zstr_st2022_5_fec_t *enc =
        zstr_st2022_5_fec_create(&(zstr_st2022_5_config_t){
            .row_len = 4, .fec_pt = 127, .col_len = 3 });
    assert(enc != NULL);

    AVPacket *media[12];
    for (int i = 0; i < 12; i++) {
        media[i] = av_packet_alloc();
        int plen = 80 + (i % 3) * 10;
        av_new_packet(media[i], 12 + plen);
        uint8_t *d = media[i]->data;
        d[0] = 0x80; d[1] = 96;
        d[2] = (uint8_t)((3000 + i) >> 8); d[3] = (uint8_t)((3000 + i) & 0xFF);
        memset(d + 4, 0, 8);
        for (int k = 0; k < plen; k++) d[12 + k] = (uint8_t)(i * 7 + k);
        media[i]->pts = 90000;
        media[i]->time_base = (AVRational){ 1, 90000 };
    }

    AVPacket *fecs[16];
    int nfec = 0;
    for (int i = 0; i < 12; i++) {
        AVPacket **arr = NULL;
        int n = 0;
        assert(zstr_st2022_5_fec_encode_matrix(enc, media[i], &arr, &n) == 0);
        if (i < 3 || (i >= 4 && i < 7) || (i >= 8 && i < 11)) assert(n == 0);
        if (i == 3 || i == 7) assert(n == 1); /* row FEC only */
        if (i == 11) assert(n == 5);          /* row + 4 column FECs */
        for (int k = 0; k < n; k++) fecs[nfec++] = arr[k];
        free(arr);
    }
    assert(nfec == 7);

    /* Column 1 FEC: base 3001, mask bits {1,5,9} */
    bool found_col = false;
    for (int i = 0; i < nfec; i++) {
        uint16_t base = (fecs[i]->data[12] << 8) | fecs[i]->data[13];
        uint32_t mask = ((uint32_t)fecs[i]->data[17] << 16) |
                        ((uint32_t)fecs[i]->data[18] << 8) | fecs[i]->data[19];
        if (base == 3001) {
            assert(mask == ((1u << 1) | (1u << 5) | (1u << 9)));
            found_col = true;
        }
    }
    assert(found_col);

    /* Drop two in row 0, distinct columns (3001=c1, 3002=c2):
     * row 0 unrecoverable, both columns recover exactly one each. */
    zstr_st2022_5_fec_decoder_t *dec =
        zstr_st2022_5_fec_decoder_create(&(zstr_st2022_5_config_t){
            .row_len = 4, .fec_pt = 127, .col_len = 3 }, 96);
    assert(dec != NULL);
    for (int i = 0; i < 12; i++) {
        if (i == 1 || i == 2) continue; /* lose seq 3001, 3002 */
        assert(zstr_st2022_5_fec_decoder_media(dec, media[i]) == 0);
    }
    AVPacket *rec1 = NULL, *rec2 = NULL;
    for (int i = 0; i < nfec; i++) {
        uint16_t fb = (fecs[i]->data[12] << 8) | fecs[i]->data[13];
        uint32_t fm = ((uint32_t)fecs[i]->data[17] << 16) |
                      ((uint32_t)fecs[i]->data[18] << 8) | fecs[i]->data[19];
        AVPacket *r = zstr_st2022_5_fec_decoder_fec(dec, fecs[i]);
        if (r) {
            uint16_t sq = (r->data[2] << 8) | r->data[3];
            if (sq == 3001) rec1 = r;
            else if (sq == 3002) rec2 = r;
            else av_packet_free(&r);
        }
    }
    assert(rec1 != NULL && rec2 != NULL);
    assert(rec1->size == media[1]->size);
    assert(memcmp(rec1->data, media[1]->data, media[1]->size) == 0);
    assert(rec2->size == media[2]->size);
    assert(memcmp(rec2->data, media[2]->data, media[2]->size) == 0);
    assert(zstr_st2022_5_fec_recovered(dec) == 2);
    av_packet_free(&rec1);
    av_packet_free(&rec2);
    zstr_st2022_5_fec_decoder_free(&dec);

    /* Single loss in matrix mode still recovers via row path */
    dec = zstr_st2022_5_fec_decoder_create(&(zstr_st2022_5_config_t){
        .row_len = 4, .fec_pt = 127, .col_len = 3 }, 96);
    for (int i = 0; i < 12; i++) {
        if (i == 6) continue; /* lose seq 3006 (row 1) */
        assert(zstr_st2022_5_fec_decoder_media(dec, media[i]) == 0);
    }
    AVPacket *rec3 = NULL;
    for (int i = 0; i < nfec; i++) {
        AVPacket *r = zstr_st2022_5_fec_decoder_fec(dec, fecs[i]);
        if (r) {
            uint16_t sq = (r->data[2] << 8) | r->data[3];
            /* Row and column paths can both fire for the same loss;
             * keep the first, free any duplicate recovery. */
            if (sq == 3006 && !rec3) rec3 = r;
            else av_packet_free(&r);
        }
    }
    assert(rec3 != NULL);
    assert(rec3->size == media[6]->size);
    assert(memcmp(rec3->data, media[6]->data, media[6]->size) == 0);
    zstr_st2022_5_fec_decoder_free(&dec);
    av_packet_free(&rec3);

    for (int i = 0; i < 12; i++) av_packet_free(&media[i]);
    for (int i = 0; i < nfec; i++) av_packet_free(&fecs[i]);
    zstr_st2022_5_fec_free(&enc);

    printf("[PASS] ST 2022-5 column FEC passed.\n");
}

static void test_st2110_device_loopback(void)
{
    printf("[TEST] Testing ST 2110 FFmpeg Device Loopback (127.0.0.1:25000)...\n");

    /* Demuxer */
    AVFormatContext *in_ctx = NULL;
    const AVInputFormat *in_fmt = zff_find_input_format("zstr_st2110_demux");
    assert(in_fmt != NULL);

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "port", "25000", 0);
    int ret = avformat_open_input(&in_ctx, "udp://127.0.0.1:25000", in_fmt, &opts);
    av_dict_free(&opts);
    assert(ret == 0 && in_ctx != NULL);

    /* Muxer */
    AVFormatContext *out_ctx = NULL;
    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_st2110_mux");
    assert(out_fmt != NULL);

    ret = avformat_alloc_output_context2(&out_ctx, out_fmt, "zstr_st2110_mux", "udp://127.0.0.1:25000");
    assert(ret >= 0 && out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->width = 320;
    st->codecpar->height = 240;

    ret = avformat_write_header(out_ctx, NULL);
    assert(ret >= 0);

    /* Write RTP packet */
    AVPacket *tx = av_packet_alloc();
    av_new_packet(tx, 120);
    memset(tx->data, 0x42, 120);
    tx->stream_index = 0;
    ret = av_write_frame(out_ctx, tx);
    assert(ret == 0);
    av_packet_free(&tx);

    /* Read packet */
    AVPacket *rx = av_packet_alloc();
    ret = av_read_frame(in_ctx, rx);
    assert(ret == 0);
    assert(rx->size == 120);
    assert(rx->data[0] == 0x42);
    av_packet_free(&rx);

    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);

    printf("[PASS] ST 2110 FFmpeg Device Loopback passed.\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    zff_plugins_register_all();

    printf("====================================================\n");
    printf("         Running SMPTE ST 2110 Suite Tests          \n");
    printf("====================================================\n");

    test_st2110_20_video_roundtrip();
    test_st2110_30_audio_roundtrip();
    test_st2022_7_redundancy();
    test_st2022_7_mux_dualsend();
    test_st2110_sdp_roundtrip();
    test_st2110_sdp_device_handoff();
    test_st2110_40_anc_roundtrip();
    test_st2110_21_narrow_pacer();
    test_st2022_5_row_fec();
    test_st2022_5_column_fec();
    test_st2110_22_rfc9134_roundtrip();
    test_st2110_22_encode_decode();
    test_st2110_device_loopback();

    printf("====================================================\n");
    printf("    All SMPTE ST 2110 Tests Passed Successfully!    \n");
    printf("====================================================\n");
    return 0;
}
