/*=============================================================================
    test_rtp.c — Unit tests for zstr_rtp_payloader and zstr_rtp_depayloader
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libavcodec/packet.h>

#include "zff/plugins/zstr_rtp.h"
#include "zff/plugins/zstr_net.h"
#include "zff/zff_core.h"

static void test_h264_single_nal(void)
{
    printf("[TEST] Testing H.264 Single NAL RTP packetization & depacketization...\n");

    zstr_rtp_payloader_t *pay = zstr_rtp_payloader_create(ZSTR_RTP_CODEC_H264, 96, 0x11223344, 90000, 1400);
    zstr_rtp_depayloader_t *depay = zstr_rtp_depayloader_create(ZSTR_RTP_CODEC_H264, 96, 90000);
    assert(pay != NULL && depay != NULL);

    /* Create small NAL packet: 00 00 00 01 67 [payload] */
    int nal_sz = 200;
    AVPacket *in_pkt = av_packet_alloc();
    av_new_packet(in_pkt, nal_sz + 4);
    in_pkt->data[0] = 0; in_pkt->data[1] = 0; in_pkt->data[2] = 0; in_pkt->data[3] = 1;
    in_pkt->data[4] = 0x67; /* SPS NAL */
    for (int i = 5; i < nal_sz + 4; i++) in_pkt->data[i] = (uint8_t)(i & 0xFF);
    in_pkt->pts = 90000;

    AVPacket **rtp_pkts = NULL;
    int nb_pkts = 0;
    int ret = zstr_rtp_payloader_process(pay, in_pkt, &rtp_pkts, &nb_pkts);
    assert(ret == 0);
    assert(nb_pkts == 1);
    assert(rtp_pkts[0] != NULL);
    assert(rtp_pkts[0]->size == ZSTR_RTP_HEADER_LEN + nal_sz);

    /* Depayload */
    AVPacket *out_pkt = av_packet_alloc();
    bool ready = false;
    ret = zstr_rtp_depayloader_process(depay, rtp_pkts[0], out_pkt, &ready);
    assert(ret == 0);
    assert(ready == true);
    assert(out_pkt->size == in_pkt->size);
    assert(memcmp(out_pkt->data, in_pkt->data, in_pkt->size) == 0);

    zstr_rtp_payloader_free_packets(rtp_pkts, nb_pkts);
    av_packet_free(&in_pkt);
    av_packet_free(&out_pkt);
    zstr_rtp_payloader_free(&pay);
    zstr_rtp_depayloader_free(&depay);

    printf("[PASS] H.264 Single NAL passed.\n");
}

static void test_h264_fu_a_fragmentation(void)
{
    printf("[TEST] Testing H.264 FU-A Fragmentation & Reassembly (5000 bytes, MTU=1200)...\n");

    zstr_rtp_payloader_t *pay = zstr_rtp_payloader_create(ZSTR_RTP_CODEC_H264, 96, 0xAABBCCDD, 90000, 1200);
    zstr_rtp_depayloader_t *depay = zstr_rtp_depayloader_create(ZSTR_RTP_CODEC_H264, 96, 90000);
    assert(pay != NULL && depay != NULL);

    int raw_nal_len = 5000;
    AVPacket *in_pkt = av_packet_alloc();
    av_new_packet(in_pkt, raw_nal_len + 4);
    in_pkt->data[0] = 0; in_pkt->data[1] = 0; in_pkt->data[2] = 0; in_pkt->data[3] = 1;
    in_pkt->data[4] = 0x65; /* IDR NAL */
    for (int i = 5; i < raw_nal_len + 4; i++) in_pkt->data[i] = (uint8_t)((i * 3) & 0xFF);
    in_pkt->pts = 180000;

    /* Add PTP Side Data */
    uint64_t ptp_timestamp_ns = 1700000000123456789ULL;
    uint8_t *sd = av_packet_new_side_data(in_pkt, ZSTR_TAG_PTP, sizeof(ptp_timestamp_ns));
    assert(sd != NULL);
    memcpy(sd, &ptp_timestamp_ns, sizeof(ptp_timestamp_ns));

    AVPacket **rtp_pkts = NULL;
    int nb_pkts = 0;
    int ret = zstr_rtp_payloader_process(pay, in_pkt, &rtp_pkts, &nb_pkts);
    assert(ret == 0);
    assert(nb_pkts > 1); /* Should be fragmented */

    AVPacket *out_pkt = av_packet_alloc();
    bool ready = false;
    for (int i = 0; i < nb_pkts; i++) {
        ret = zstr_rtp_depayloader_process(depay, rtp_pkts[i], out_pkt, &ready);
        assert(ret == 0);
        if (i < nb_pkts - 1) {
            assert(ready == false);
        } else {
            assert(ready == true);
        }
    }

    assert(out_pkt->size == in_pkt->size);
    assert(memcmp(out_pkt->data, in_pkt->data, in_pkt->size) == 0);

    /* Verify PTP Side Data was preserved */
    size_t sd_sz = 0;
    uint8_t *rx_sd = av_packet_get_side_data(out_pkt, ZSTR_TAG_PTP, &sd_sz);
    assert(rx_sd != NULL && sd_sz == sizeof(uint64_t));
    uint64_t rx_ptp = 0;
    memcpy(&rx_ptp, rx_sd, sizeof(rx_ptp));
    assert(rx_ptp == ptp_timestamp_ns);

    zstr_rtp_payloader_free_packets(rtp_pkts, nb_pkts);
    av_packet_free(&in_pkt);
    av_packet_free(&out_pkt);
    zstr_rtp_payloader_free(&pay);
    zstr_rtp_depayloader_free(&depay);

    printf("[PASS] H.264 FU-A Fragmentation & Reassembly passed.\n");
}

static void test_rtp_udp_loopback_integration(void)
{
    printf("[TEST] Testing End-to-End RTP over UDP Network Loopback (127.0.0.1:15008)...\n");

    zstr_net_config_t src_cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = "127.0.0.1",
        .port = 15008,
        .buffer_size = 65536,
        .timeout_ms = 1000
    };
    zstr_net_source_t *net_src = zstr_net_source_create(&src_cfg);
    assert(net_src != NULL);

    zstr_net_config_t sink_cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = "127.0.0.1",
        .port = 15008,
        .buffer_size = 65536,
        .timeout_ms = 1000
    };
    zstr_net_sink_t *net_sink = zstr_net_sink_create(&sink_cfg);
    assert(net_sink != NULL);

    zstr_rtp_payloader_t *pay = zstr_rtp_payloader_create(ZSTR_RTP_CODEC_H264, 96, 0xCAFEBABE, 90000, 1400);
    zstr_rtp_depayloader_t *depay = zstr_rtp_depayloader_create(ZSTR_RTP_CODEC_H264, 96, 90000);

    /* 4000 byte video frame */
    int frame_size = 4000;
    AVPacket *tx_frame = av_packet_alloc();
    av_new_packet(tx_frame, frame_size + 4);
    tx_frame->data[0] = 0; tx_frame->data[1] = 0; tx_frame->data[2] = 0; tx_frame->data[3] = 1;
    tx_frame->data[4] = 0x61; /* Non-IDR Slice */
    for (int i = 5; i < frame_size + 4; i++) tx_frame->data[i] = (uint8_t)(i ^ 0xA5);
    tx_frame->pts = 270000;

    /* Payloader -> RTP packets */
    AVPacket **rtp_pkts = NULL;
    int nb_pkts = 0;
    int ret = zstr_rtp_payloader_process(pay, tx_frame, &rtp_pkts, &nb_pkts);
    assert(ret == 0 && nb_pkts > 1);

    /* Send RTP packets over UDP */
    for (int i = 0; i < nb_pkts; i++) {
        ret = zstr_net_sink_write_packet(net_sink, rtp_pkts[i]);
        assert(ret > 0);
    }

    /* Receive RTP packets over UDP and Depayload */
    AVPacket *rx_frame = av_packet_alloc();
    bool ready = false;
    for (int i = 0; i < nb_pkts; i++) {
        AVPacket *rx_rtp = av_packet_alloc();
        ret = zstr_net_source_read_packet(net_src, rx_rtp);
        assert(ret == 0);

        ret = zstr_rtp_depayloader_process(depay, rx_rtp, rx_frame, &ready);
        assert(ret == 0);
        av_packet_free(&rx_rtp);
    }
    assert(ready == true);

    /* Verify payload data */
    assert(rx_frame->size == tx_frame->size);
    assert(memcmp(rx_frame->data, tx_frame->data, tx_frame->size) == 0);

    zstr_rtp_payloader_free_packets(rtp_pkts, nb_pkts);
    av_packet_free(&tx_frame);
    av_packet_free(&rx_frame);
    zstr_rtp_payloader_free(&pay);
    zstr_rtp_depayloader_free(&depay);
    zstr_net_source_close(&net_src);
    zstr_net_sink_close(&net_sink);

    printf("[PASS] End-to-End RTP over UDP Network Loopback passed.\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    zff_plugins_register_all();

    printf("====================================================\n");
    printf("     Running zstr_rtp Payloader/Depayloader Tests   \n");
    printf("====================================================\n");

    test_h264_single_nal();
    test_h264_fu_a_fragmentation();
    test_rtp_udp_loopback_integration();

    printf("====================================================\n");
    printf("    All zstr_rtp Tests Passed Successfully!         \n");
    printf("====================================================\n");
    return 0;
}
