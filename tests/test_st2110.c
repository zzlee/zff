/*=============================================================================
    test_st2110.c — Unit tests for SMPTE ST 2110 Broadcast Suite & ST 2022-7
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>

#include "zff/plugins/zstr_st2110.h"
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
    test_st2110_device_loopback();

    printf("====================================================\n");
    printf("    All SMPTE ST 2110 Tests Passed Successfully!    \n");
    printf("====================================================\n");
    return 0;
}
