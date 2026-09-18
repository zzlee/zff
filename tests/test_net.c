/*=============================================================================
    test_net.c — Unit tests for zstr_net_src and zstr_net_sink FFmpeg devices
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <libavformat/avformat.h>

#include <stdbool.h>
#include "zff/plugins/zstr_net.h"
#include "zff/zff_core.h"

/* ---------------------------------------------------------------------------
 * Test 1: UDP FFmpeg Device Loopback
 * --------------------------------------------------------------------------- */
static void test_udp_device_loopback(void)
{
    printf("[TEST] Testing UDP FFmpeg device loopback (udp://127.0.0.1:15004)...\n");

    const AVInputFormat *in_fmt = zff_find_input_format("zstr_net_src");
    assert(in_fmt != NULL);

    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_net_sink");
    assert(out_fmt != NULL);

    /* Receiver */
    AVFormatContext *in_ctx = NULL;
    AVDictionary *in_opts = NULL;
    av_dict_set(&in_opts, "port", "15004", 0);
    av_dict_set(&in_opts, "timeout", "1000", 0);
    int ret = avformat_open_input(&in_ctx, "udp://127.0.0.1:15004", in_fmt, &in_opts);
    av_dict_free(&in_opts);
    assert(ret == 0 && in_ctx != NULL);

    /* Sender */
    AVFormatContext *out_ctx = NULL;
    ret = avformat_alloc_output_context2(&out_ctx, out_fmt, "zstr_net_sink", "udp://127.0.0.1:15004");
    assert(ret >= 0 && out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->codecpar->codec_id = AV_CODEC_ID_NONE;

    ret = avformat_write_header(out_ctx, NULL);
    assert(ret >= 0);

    /* Send and receive multiple packets */
    for (int i = 0; i < 5; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "UDP_PAYLOAD_%d", i);

        AVPacket *tx = av_packet_alloc();
        av_new_packet(tx, (int)strlen(msg) + 1);
        memcpy(tx->data, msg, strlen(msg) + 1);
        tx->stream_index = 0;
        tx->pts = 1000 + i;

        ret = av_write_frame(out_ctx, tx);
        assert(ret == 0);
        av_packet_free(&tx);

        AVPacket *rx = av_packet_alloc();
        ret = av_read_frame(in_ctx, rx);
        assert(ret == 0);
        assert(rx->size == (int)strlen(msg) + 1);
        assert(strcmp((char*)rx->data, msg) == 0);
        assert(rx->pts > 0);
        av_packet_free(&rx);
    }

    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);

    printf("[PASS] UDP FFmpeg device loopback passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 2: TCP Server/Client FFmpeg Device Loopback
 * --------------------------------------------------------------------------- */
typedef struct {
    AVFormatContext *in_ctx;
    AVPacket *rx_pkt;
    int ret;
    volatile bool done;
} TCPThreadArg;

static void* tcp_server_thread(void *arg)
{
    TCPThreadArg *t = (TCPThreadArg*)arg;
    t->ret = av_read_frame(t->in_ctx, t->rx_pkt);
    t->done = true;
    return NULL;
}

static void test_tcp_device_loopback(void)
{
    printf("[TEST] Testing TCP Server/Client FFmpeg device loopback (tcp://127.0.0.1:15005)...\n");

    const AVInputFormat *in_fmt = zff_find_input_format("zstr_net_src");
    assert(in_fmt != NULL);

    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_net_sink");
    assert(out_fmt != NULL);

    /* Start TCP Server Receiver */
    AVFormatContext *in_ctx = NULL;
    AVDictionary *in_opts = NULL;
    av_dict_set(&in_opts, "protocol", "tcp_server", 0);
    av_dict_set(&in_opts, "port", "15005", 0);
    av_dict_set(&in_opts, "timeout", "3000", 0);

    int ret = avformat_open_input(&in_ctx, "tcp://127.0.0.1:15005", in_fmt, &in_opts);
    av_dict_free(&in_opts);
    assert(ret == 0 && in_ctx != NULL);

    AVPacket *rx = av_packet_alloc();
    TCPThreadArg t_arg = { .in_ctx = in_ctx, .rx_pkt = rx, .ret = -1, .done = false };
    pthread_t th;
    pthread_create(&th, NULL, tcp_server_thread, &t_arg);

    usleep(20000); /* 20ms wait for server to listen */

    /* Start TCP Client Sender */
    AVFormatContext *out_ctx = NULL;
    AVDictionary *out_opts = NULL;
    av_dict_set(&out_opts, "protocol", "tcp_client", 0);
    av_dict_set(&out_opts, "port", "15005", 0);
    av_dict_set(&out_opts, "timeout", "3000", 0);

    ret = avformat_alloc_output_context2(&out_ctx, out_fmt, "zstr_net_sink", "tcp://127.0.0.1:15005");
    assert(ret >= 0 && out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;

    ret = avformat_write_header(out_ctx, &out_opts);
    av_dict_free(&out_opts);
    assert(ret >= 0);

    const char *payload = "HELLO_TCP_FFMPEG_WORLD";
    AVPacket *tx = av_packet_alloc();
    av_new_packet(tx, (int)strlen(payload) + 1);
    memcpy(tx->data, payload, strlen(payload) + 1);
    tx->stream_index = 0;
    tx->pts = 42;

    ret = av_write_frame(out_ctx, tx);
    assert(ret == 0);
    av_packet_free(&tx);

    pthread_join(th, NULL);
    assert(t_arg.done == true);
    assert(t_arg.ret == 0);
    assert(rx->size == (int)strlen(payload) + 1);
    assert(strcmp((char*)rx->data, payload) == 0);

    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);
    av_packet_free(&rx);

    printf("[PASS] TCP Server/Client FFmpeg device loopback passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 3: Option & URL Parsing
 * --------------------------------------------------------------------------- */
static void test_net_options_and_url(void)
{
    printf("[TEST] Testing net device options and URL parsing...\n");

    const AVInputFormat *in_fmt = zff_find_input_format("zstr_net_src");
    assert(in_fmt != NULL);

    AVFormatContext *in_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "buffer_size", "32768", 0);
    av_dict_set(&opts, "timeout", "500", 0);

    int ret = avformat_open_input(&in_ctx, "udp://127.0.0.1:15007", in_fmt, &opts);
    av_dict_free(&opts);
    assert(ret == 0 && in_ctx != NULL);

    avformat_close_input(&in_ctx);
    printf("[PASS] Net device options and URL parsing passed.\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    zff_plugins_register_all();

    printf("====================================================\n");
    printf("     Running zstr_net FFmpeg Pure Device Tests      \n");
    printf("====================================================\n");

    test_udp_device_loopback();
    test_tcp_device_loopback();
    test_net_options_and_url();

    printf("\nAll zstr_net device tests passed successfully!\n");
    return 0;
}
