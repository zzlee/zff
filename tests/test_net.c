/*=============================================================================
    test_net.c — Unit tests for zstr_net_source and zstr_net_sink
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <libavformat/avformat.h>

#include "zff/plugins/zstr_net.h"
#include "zff/zff_core.h"

static void test_udp_direct_loopback(void)
{
    printf("[TEST] Testing UDP direct loopback (127.0.0.1:15004)...\n");

    zstr_net_config_t src_cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = "127.0.0.1",
        .port = 15004,
        .buffer_size = 65536,
        .timeout_ms = 1000
    };
    zstr_net_source_t *src = zstr_net_source_create(&src_cfg);
    assert(src != NULL);

    zstr_net_config_t sink_cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = "127.0.0.1",
        .port = 15004,
        .buffer_size = 65536,
        .timeout_ms = 1000
    };
    zstr_net_sink_t *sink = zstr_net_sink_create(&sink_cfg);
    assert(sink != NULL);

    /* Send test packets */
    for (int i = 0; i < 5; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "UDP_PAYLOAD_%d", i);

        AVPacket *tx_pkt = av_packet_alloc();
        av_new_packet(tx_pkt, (int)strlen(msg) + 1);
        memcpy(tx_pkt->data, msg, strlen(msg) + 1);

        int sent = zstr_net_sink_write_packet(sink, tx_pkt);
        assert(sent > 0);
        av_packet_free(&tx_pkt);

        AVPacket *rx_pkt = av_packet_alloc();
        int ret = zstr_net_source_read_packet(src, rx_pkt);
        assert(ret == 0);
        assert(rx_pkt->size == (int)strlen(msg) + 1);
        assert(strcmp((char*)rx_pkt->data, msg) == 0);
        assert(rx_pkt->pts > 0);
        av_packet_free(&rx_pkt);
    }

    zstr_net_source_close(&src);
    zstr_net_sink_close(&sink);
    assert(src == NULL && sink == NULL);
    printf("[PASS] UDP direct loopback passed.\n");
}

static void* tcp_server_thread(void *arg)
{
    zstr_net_source_t *src = (zstr_net_source_t*)arg;
    uint8_t buf[128];
    int n = zstr_net_source_read(src, buf, sizeof(buf));
    assert(n > 0);
    assert(strcmp((char*)buf, "HELLO_TCP_WORLD") == 0);
    return NULL;
}

static void test_tcp_direct_loopback(void)
{
    printf("[TEST] Testing TCP client/server loopback (127.0.0.1:15005)...\n");

    zstr_net_config_t srv_cfg = {
        .protocol = ZSTR_NET_PROTO_TCP_SERVER,
        .host = "127.0.0.1",
        .port = 15005,
        .buffer_size = 65536,
        .timeout_ms = 2000
    };
    zstr_net_source_t *src = zstr_net_source_create(&srv_cfg);
    assert(src != NULL);

    pthread_t th;
    pthread_create(&th, NULL, tcp_server_thread, src);
    usleep(10000); /* 10ms */

    zstr_net_config_t cli_cfg = {
        .protocol = ZSTR_NET_PROTO_TCP_CLIENT,
        .host = "127.0.0.1",
        .port = 15005,
        .buffer_size = 65536,
        .timeout_ms = 2000
    };
    zstr_net_sink_t *sink = zstr_net_sink_create(&cli_cfg);
    assert(sink != NULL);

    const char *msg = "HELLO_TCP_WORLD";
    int sent = zstr_net_sink_write(sink, (const uint8_t*)msg, (int)strlen(msg) + 1);
    assert(sent == (int)strlen(msg) + 1);

    pthread_join(th, NULL);

    zstr_net_source_close(&src);
    zstr_net_sink_close(&sink);
    printf("[PASS] TCP client/server loopback passed.\n");
}

static void test_ffmpeg_net_device_loopback(void)
{
    printf("[TEST] Testing FFmpeg AVInputFormat/AVOutputFormat device loopback (127.0.0.1:15006)...\n");

    /* Receiver format context */
    AVFormatContext *in_ctx = NULL;
    const AVInputFormat *in_fmt = zff_find_input_format("zstr_net_src");
    assert(in_fmt != NULL);

    AVDictionary *in_opts = NULL;
    av_dict_set(&in_opts, "port", "15006", 0);
    int ret = avformat_open_input(&in_ctx, "udp://127.0.0.1:15006", in_fmt, &in_opts);
    av_dict_free(&in_opts);
    assert(ret == 0 && in_ctx != NULL);

    /* Sender format context */
    AVFormatContext *out_ctx = NULL;
    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_net_sink");
    assert(out_fmt != NULL);

    ret = avformat_alloc_output_context2(&out_ctx, out_fmt, "zstr_net_sink", "udp://127.0.0.1:15006");
    assert(ret >= 0 && out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->codecpar->codec_id = AV_CODEC_ID_NONE;

    ret = avformat_write_header(out_ctx, NULL);
    assert(ret >= 0);

    /* Write packet */
    const char *payload = "FFMPEG_NETWORK_PACKET";
    AVPacket *tx = av_packet_alloc();
    av_new_packet(tx, (int)strlen(payload) + 1);
    tx->stream_index = 0;
    memcpy(tx->data, payload, strlen(payload) + 1);
    tx->pts = 12345;

    ret = av_write_frame(out_ctx, tx);
    assert(ret == 0);
    av_packet_free(&tx);

    /* Read packet */
    AVPacket *rx = av_packet_alloc();
    ret = av_read_frame(in_ctx, rx);
    assert(ret == 0);
    assert(rx->size == (int)strlen(payload) + 1);
    assert(strcmp((char*)rx->data, payload) == 0);
    av_packet_free(&rx);

    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);

    printf("[PASS] FFmpeg AVInputFormat/AVOutputFormat device loopback passed.\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    zff_plugins_register_all();

    printf("====================================================\n");
    printf("         Running zstr_net Source/Sink Tests         \n");
    printf("====================================================\n");

    test_udp_direct_loopback();
    test_tcp_direct_loopback();
    test_ffmpeg_net_device_loopback();

    printf("====================================================\n");
    printf("    All zstr_net Tests Passed Successfully!         \n");
    printf("====================================================\n");
    return 0;
}
