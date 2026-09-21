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

static void test_h264_multi_nal_au(void)
{
    printf("[TEST] Testing H.264 multi-NAL access unit (SPS+PPS+IDR slice)...\n");

    zstr_rtp_payloader_t *pay = zstr_rtp_payloader_create(ZSTR_RTP_CODEC_H264, 96, 0x55667788, 90000, 1400);
    zstr_rtp_depayloader_t *depay = zstr_rtp_depayloader_create(ZSTR_RTP_CODEC_H264, 96, 90000);
    assert(pay != NULL && depay != NULL);

    /* AU: small SPS + small PPS + large IDR (forces FU-A) */
    uint8_t au[64 + 32 + 3004];
    int pos = 0;
    au[pos++] = 0; au[pos++] = 0; au[pos++] = 0; au[pos++] = 1;
    au[pos++] = 0x67;
    for (int i = 0; i < 59; i++) au[pos++] = (uint8_t)(0x10 + i);
    au[pos++] = 0; au[pos++] = 0; au[pos++] = 0; au[pos++] = 1;
    au[pos++] = 0x68;
    for (int i = 0; i < 27; i++) au[pos++] = (uint8_t)(0x30 + i);
    au[pos++] = 0; au[pos++] = 0; au[pos++] = 0; au[pos++] = 1;
    au[pos++] = 0x65;
    for (int i = 0; i < 2999; i++) au[pos++] = (uint8_t)((i * 7) & 0xFF);
    int au_len = pos;

    AVPacket *in_pkt = av_packet_alloc();
    av_new_packet(in_pkt, au_len);
    memcpy(in_pkt->data, au, au_len);
    in_pkt->pts = 360000;
    in_pkt->time_base = (AVRational){ 1, 90000 };

    AVPacket **rtp_pkts = NULL;
    int nb_pkts = 0;
    int ret = zstr_rtp_payloader_process(pay, in_pkt, &rtp_pkts, &nb_pkts);
    assert(ret == 0);
    assert(nb_pkts == 2 + 3); /* SPS + PPS single, IDR 2999B -> 3 FU-A frags at MTU 1400 */
    /* Marker only on the last packet of the access unit */
    for (int i = 0; i < nb_pkts; i++) {
        bool m = (rtp_pkts[i]->data[1] & 0x80) != 0;
        assert(m == (i == nb_pkts - 1));
    }
    /* Same RTP timestamp across the whole AU */
    uint32_t ts0;
    memcpy(&ts0, rtp_pkts[0]->data + 4, 4);
    for (int i = 1; i < nb_pkts; i++)
        assert(memcmp(rtp_pkts[i]->data + 4, &ts0, 4) == 0);

    /* Depayload reassembles the full AU */
    AVPacket *out_pkt = av_packet_alloc();
    bool ready = false;
    for (int i = 0; i < nb_pkts; i++) {
        ret = zstr_rtp_depayloader_process(depay, rtp_pkts[i], out_pkt, &ready);
        assert(ret == 0);
        assert(ready == (i == nb_pkts - 1));
    }
    assert(ready == true);
    assert(out_pkt->size == in_pkt->size);
    assert(memcmp(out_pkt->data, in_pkt->data, in_pkt->size) == 0);

    zstr_rtp_payloader_free_packets(rtp_pkts, nb_pkts);
    av_packet_free(&in_pkt);
    av_packet_free(&out_pkt);
    zstr_rtp_payloader_free(&pay);
    zstr_rtp_depayloader_free(&depay);

    printf("[PASS] H.264 multi-NAL access unit passed.\n");
}

static void test_aac_rfc3640(void)
{
    printf("[TEST] Testing AAC RFC 3640 framing & reassembly...\n");

    zstr_rtp_payloader_t *pay = zstr_rtp_payloader_create(ZSTR_RTP_CODEC_AAC, 97, 0x99AABBCC, 44100, 1400);
    zstr_rtp_depayloader_t *depay = zstr_rtp_depayloader_create(ZSTR_RTP_CODEC_AAC, 97, 44100);
    assert(pay != NULL && depay != NULL);

    int raw_len = 256;
    AVPacket *in_pkt = av_packet_alloc();
    av_new_packet(in_pkt, raw_len);
    for (int i = 0; i < raw_len; i++) in_pkt->data[i] = (uint8_t)(0x21 + i);
    in_pkt->pts = 1024;
    in_pkt->time_base = (AVRational){ 1, 44100 };

    AVPacket **rtp_pkts = NULL;
    int nb_pkts = 0;
    int ret = zstr_rtp_payloader_process(pay, in_pkt, &rtp_pkts, &nb_pkts);
    assert(ret == 0);
    assert(nb_pkts == 1);
    /* PT=97 with marker */
    assert((rtp_pkts[0]->data[1] & 0x7F) == 97);
    assert((rtp_pkts[0]->data[1] & 0x80) != 0);
    /* AU-headers-length = 16 bits */
    assert(rtp_pkts[0]->data[12] == 0 && rtp_pkts[0]->data[13] == 16);
    /* AU-header size field = raw_len << 3 */
    uint16_t au = (uint16_t)((rtp_pkts[0]->data[14] << 8) | rtp_pkts[0]->data[15]);
    assert(au == (uint16_t)(raw_len << 3));
    assert(rtp_pkts[0]->size == ZSTR_RTP_HEADER_LEN + 4 + raw_len);

    AVPacket *out_pkt = av_packet_alloc();
    bool ready = false;
    ret = zstr_rtp_depayloader_process(depay, rtp_pkts[0], out_pkt, &ready);
    assert(ret == 0);
    assert(ready == true);
    assert(out_pkt->size == raw_len);
    assert(memcmp(out_pkt->data, in_pkt->data, raw_len) == 0);

    zstr_rtp_payloader_free_packets(rtp_pkts, nb_pkts);
    av_packet_free(&in_pkt);
    av_packet_free(&out_pkt);
    zstr_rtp_payloader_free(&pay);
    zstr_rtp_depayloader_free(&depay);

    printf("[PASS] AAC RFC 3640 framing passed.\n");
}

static void test_rtp_udp_loopback_integration(void)
{
    printf("[TEST] Testing End-to-End RTP over UDP Network Loopback (127.0.0.1:15008)...\n");

    const AVInputFormat *in_fmt = zff_find_input_format("zstr_net_src");
    assert(in_fmt != NULL);

    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_net_sink");
    assert(out_fmt != NULL);

    AVFormatContext *in_ctx = NULL;
    AVDictionary *in_opts = NULL;
    av_dict_set(&in_opts, "port", "15008", 0);
    av_dict_set(&in_opts, "timeout", "1000", 0);
    int ret = avformat_open_input(&in_ctx, "udp://127.0.0.1:15008", in_fmt, &in_opts);
    av_dict_free(&in_opts);
    assert(ret == 0 && in_ctx != NULL);

    AVFormatContext *out_ctx = NULL;
    ret = avformat_alloc_output_context2(&out_ctx, out_fmt, "zstr_net_sink", "udp://127.0.0.1:15008");
    assert(ret >= 0 && out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    ret = avformat_write_header(out_ctx, NULL);
    assert(ret >= 0);

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
    ret = zstr_rtp_payloader_process(pay, tx_frame, &rtp_pkts, &nb_pkts);
    assert(ret == 0 && nb_pkts > 1);

    /* Send RTP packets over UDP */
    for (int i = 0; i < nb_pkts; i++) {
        rtp_pkts[i]->stream_index = 0;
        ret = av_write_frame(out_ctx, rtp_pkts[i]);
        assert(ret == 0);
    }

    /* Receive RTP packets over UDP and Depayload */
    AVPacket *rx_frame = av_packet_alloc();
    bool ready = false;
    for (int i = 0; i < nb_pkts; i++) {
        AVPacket *rx_rtp = av_packet_alloc();
        ret = av_read_frame(in_ctx, rx_rtp);
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

    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);

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
    test_h264_multi_nal_au();
    test_aac_rfc3640();
    test_rtp_udp_loopback_integration();

    printf("====================================================\n");
    printf("    All zstr_rtp Tests Passed Successfully!         \n");
    printf("====================================================\n");
    return 0;
}
