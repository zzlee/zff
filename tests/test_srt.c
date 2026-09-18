/*=============================================================================
    test_srt.c — Comprehensive Unit Tests for SRT Subtitle Parser & Streaming
=============================================================================*/
#include "zff/plugins/zstr_srt.h"
#include "zff/plugins/zstr_text_overlay.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

/* ---------------------------------------------------------------------------
 * Test 1: Subtitle Parser (Memory & File parsing)
 * --------------------------------------------------------------------------- */
static const char *k_sample_srt =
    "1\n"
    "00:00:01,000 --> 00:00:03,500\n"
    "Hello World!\n"
    "Welcome to zstreamer FFmpeg bridge.\n"
    "\n"
    "2\n"
    "00:00:05.200 --> 00:00:08.000\n"
    "Secure Reliable Transport active.\n"
    "\n";

static void test_srt_parser_memory(void)
{
    printf("[TEST] Testing SRT Subtitle Parser from memory...\n");

    zstr_srt_parser_t *p = zstr_srt_parser_create_from_memory(k_sample_srt, strlen(k_sample_srt));
    assert(p != NULL);

    int count = zstr_srt_parser_get_count(p);
    assert(count == 2);

    int64_t start_ms = 0, dur_ms = 0;
    const char *text = NULL;

    int ret = zstr_srt_parser_get_entry(p, 0, &start_ms, &dur_ms, &text);
    assert(ret == 0);
    assert(start_ms == 1000);
    assert(dur_ms == 2500);
    assert(strstr(text, "Hello World!") != NULL);
    assert(strstr(text, "Welcome to zstreamer") != NULL);

    ret = zstr_srt_parser_get_entry(p, 1, &start_ms, &dur_ms, &text);
    assert(ret == 0);
    assert(start_ms == 5200);
    assert(dur_ms == 2800);
    assert(strcmp(text, "Secure Reliable Transport active.") == 0);

    /* Test timestamp lookup */
    assert(zstr_srt_parser_find_at_time(p, 500) == NULL);
    assert(zstr_srt_parser_find_at_time(p, 1000) != NULL);
    assert(zstr_srt_parser_find_at_time(p, 2500) != NULL);
    assert(zstr_srt_parser_find_at_time(p, 3500) == NULL);
    assert(zstr_srt_parser_find_at_time(p, 4000) == NULL);
    assert(zstr_srt_parser_find_at_time(p, 5200) != NULL);
    assert(zstr_srt_parser_find_at_time(p, 7999) != NULL);
    assert(zstr_srt_parser_find_at_time(p, 8000) == NULL);

    zstr_srt_parser_free(&p);
    assert(p == NULL);
    printf("[PASS] SRT Subtitle Parser from memory passed.\n");
}

static void test_srt_parser_file(void)
{
    printf("[TEST] Testing SRT Subtitle Parser from file...\n");
    const char *filepath = "/tmp/test_zff_subtitle.srt";
    FILE *f = fopen(filepath, "w");
    assert(f != NULL);
    fputs(k_sample_srt, f);
    fclose(f);

    zstr_srt_parser_t *p = zstr_srt_parser_create_from_file(filepath);
    assert(p != NULL);
    assert(zstr_srt_parser_get_count(p) == 2);

    unlink(filepath);
    zstr_srt_parser_free(&p);
    printf("[PASS] SRT Subtitle Parser from file passed.\n");
}

static void test_srt_parser_overlay_integration(void)
{
    printf("[TEST] Testing SRT Subtitle Parser overlay integration...\n");

    zstr_srt_parser_t *p = zstr_srt_parser_create_from_memory(k_sample_srt, strlen(k_sample_srt));
    assert(p != NULL);

    zstr_text_overlay_config_t cfg = {
        .text = NULL,
        .x = 20,
        .y = 40,
        .font_size = 24,
        .text_color = 0xFFFFFFFF,
        .draw_box = true,
        .box_color = 0x00000080
    };
    zstr_text_overlay_t *overlay = zstr_text_overlay_create(&cfg);
    assert(overlay != NULL);

    AVRational tb = (AVRational){ 1, 1000 };

    /* At time 500ms: no subtitle */
    int ret = zstr_srt_parser_apply_to_overlay(p, overlay, 500, tb);
    assert(ret == 0);

    /* Render on a test frame */
    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 320;
    frame->height = 240;
    frame->pts = 500;
    assert(av_frame_get_buffer(frame, 0) == 0);
    memset(frame->data[0], 0x10, frame->linesize[0] * 240);
    memset(frame->data[1], 0x80, frame->linesize[1] * 120);
    memset(frame->data[2], 0x80, frame->linesize[2] * 120);

    ret = zstr_text_overlay_process(overlay, frame, frame);
    assert(ret == 0);

    /* At time 2000ms: subtitle 1 is active */
    ret = zstr_srt_parser_apply_to_overlay(p, overlay, 2000, tb);
    assert(ret == 0);
    frame->pts = 2000;
    ret = zstr_text_overlay_process(overlay, frame, frame);
    assert(ret == 0);

    /* At time 6000ms: subtitle 2 is active */
    ret = zstr_srt_parser_apply_to_overlay(p, overlay, 6000, tb);
    assert(ret == 0);
    frame->pts = 6000;
    ret = zstr_text_overlay_process(overlay, frame, frame);
    assert(ret == 0);

    av_frame_free(&frame);
    zstr_text_overlay_free(&overlay);
    zstr_srt_parser_free(&p);
    printf("[PASS] SRT Subtitle Parser overlay integration passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 2: SRT Network Transport Direct C API Loopback
 * --------------------------------------------------------------------------- */
typedef struct {
    zstr_srt_source_t *src;
    uint8_t received_data[2048];
    int received_len;
    volatile bool done;
} ReceiverArg;

static void* srt_receiver_thread(void *arg)
{
    ReceiverArg *r = (ReceiverArg*)arg;
    int total = 0;
    while (total < 1000) {
        int n = zstr_srt_source_read(r->src, r->received_data + total, (int)sizeof(r->received_data) - total);
        if (n <= 0) break;
        total += n;
    }
    r->received_len = total;
    r->done = true;
    return NULL;
}

static void test_srt_streaming_direct_loopback(void)
{
    printf("[TEST] Testing SRT Network Transport Direct C API Loopback...\n");

    zstr_srt_config_t src_cfg = {
        .mode = ZSTR_SRT_MODE_LISTENER,
        .host = "127.0.0.1",
        .port = 19120,
        .latency_ms = 50,
        .timeout_ms = 3000
    };
    zstr_srt_source_t *src = zstr_srt_source_create(&src_cfg);
    assert(src != NULL);

    ReceiverArg rx_arg = { .src = src, .received_len = 0, .done = false };
    pthread_t th;
    pthread_create(&th, NULL, srt_receiver_thread, &rx_arg);

    /* Wait briefly for listener to be ready */
    usleep(50000);

    zstr_srt_config_t sink_cfg = {
        .mode = ZSTR_SRT_MODE_CALLER,
        .host = "127.0.0.1",
        .port = 19120,
        .latency_ms = 50,
        .timeout_ms = 3000
    };
    zstr_srt_sink_t *sink = zstr_srt_sink_create(&sink_cfg);
    assert(sink != NULL);

    uint8_t tx_buf[1000];
    for (int i = 0; i < 1000; i++) tx_buf[i] = (uint8_t)(i & 0xFF);

    int written = zstr_srt_sink_write(sink, tx_buf, sizeof(tx_buf));
    assert(written == sizeof(tx_buf));

    pthread_join(th, NULL);
    assert(rx_arg.done == true);
    assert(rx_arg.received_len == sizeof(tx_buf));
    assert(memcmp(rx_arg.received_data, tx_buf, sizeof(tx_buf)) == 0);

    zstr_srt_sink_close(&sink);
    zstr_srt_source_close(&src);
    assert(sink == NULL && src == NULL);

    printf("[PASS] SRT Network Transport Direct C API Loopback passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 3: SRT Encrypted Transmission (AES Passphrase)
 * --------------------------------------------------------------------------- */
static void test_srt_encrypted_loopback(void)
{
    printf("[TEST] Testing SRT Encrypted Loopback with AES passphrase...\n");

    const char *passphrase = "zstreamer_secure_aes_key";
    zstr_srt_config_t src_cfg = {
        .mode = ZSTR_SRT_MODE_LISTENER,
        .host = "127.0.0.1",
        .port = 19122,
        .latency_ms = 50,
        .passphrase = passphrase,
        .pbkeylen = 16,
        .timeout_ms = 3000
    };
    zstr_srt_source_t *src = zstr_srt_source_create(&src_cfg);
    assert(src != NULL);

    ReceiverArg rx_arg = { .src = src, .received_len = 0, .done = false };
    pthread_t th;
    pthread_create(&th, NULL, srt_receiver_thread, &rx_arg);

    usleep(50000);

    zstr_srt_config_t sink_cfg = {
        .mode = ZSTR_SRT_MODE_CALLER,
        .host = "127.0.0.1",
        .port = 19122,
        .latency_ms = 50,
        .passphrase = passphrase,
        .pbkeylen = 16,
        .timeout_ms = 3000
    };
    zstr_srt_sink_t *sink = zstr_srt_sink_create(&sink_cfg);
    assert(sink != NULL);

    uint8_t tx_buf[1000];
    for (int i = 0; i < 1000; i++) tx_buf[i] = (uint8_t)((i ^ 0xAA) & 0xFF);

    int written = zstr_srt_sink_write(sink, tx_buf, sizeof(tx_buf));
    assert(written == sizeof(tx_buf));

    pthread_join(th, NULL);
    assert(rx_arg.done == true);
    assert(rx_arg.received_len == sizeof(tx_buf));
    assert(memcmp(rx_arg.received_data, tx_buf, sizeof(tx_buf)) == 0);

    zstr_srt_sink_close(&sink);
    zstr_srt_source_close(&src);

    printf("[PASS] SRT Encrypted Loopback passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 4: FFmpeg AVInputFormat / AVOutputFormat Device Loopback
 * --------------------------------------------------------------------------- */
typedef struct {
    AVFormatContext *in_ctx;
    AVPacket *pkt;
    int result;
    volatile bool done;
} FFDemuxArg;

static void* ff_demux_thread(void *arg)
{
    FFDemuxArg *d = (FFDemuxArg*)arg;
    d->result = av_read_frame(d->in_ctx, d->pkt);
    d->done = true;
    return NULL;
}

static void test_srt_ffmpeg_device_loopback(void)
{
    printf("[TEST] Testing SRT FFmpeg Device Loopback (zstr_srt_src & zstr_srt_sink)...\n");

    const AVInputFormat *in_fmt = zff_find_input_format("zstr_srt_src");
    assert(in_fmt != NULL);

    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_srt_sink");
    assert(out_fmt != NULL);

    /* Open Demuxer (Listener on port 19124) */
    AVFormatContext *in_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "mode", "listener", 0);
    av_dict_set(&opts, "port", "19124", 0);
    av_dict_set(&opts, "latency", "50", 0);

    int ret = avformat_open_input(&in_ctx, "srt://127.0.0.1:19124?mode=listener&latency=50", in_fmt, &opts);
    av_dict_free(&opts);
    assert(ret == 0 && in_ctx != NULL);

    AVPacket *rx_pkt = av_packet_alloc();
    FFDemuxArg demux_arg = { .in_ctx = in_ctx, .pkt = rx_pkt, .result = -1, .done = false };
    pthread_t th;
    pthread_create(&th, NULL, ff_demux_thread, &demux_arg);

    usleep(50000);

    /* Open Muxer (Caller to port 19124) */
    AVFormatContext *out_ctx = NULL;
    ret = avformat_alloc_output_context2(&out_ctx, out_fmt, "zstr_srt_sink", "srt://127.0.0.1:19124?mode=caller&latency=50");
    assert(ret >= 0 && out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;

    ret = avformat_write_header(out_ctx, NULL);
    assert(ret >= 0);

    AVPacket *tx_pkt = av_packet_alloc();
    av_new_packet(tx_pkt, 512);
    memset(tx_pkt->data, 0x5A, 512);
    tx_pkt->stream_index = 0;

    ret = av_write_frame(out_ctx, tx_pkt);
    assert(ret == 0);

    pthread_join(th, NULL);
    assert(demux_arg.done == true);
    assert(demux_arg.result == 0);
    assert(rx_pkt->size == 512);
    assert(rx_pkt->data[0] == 0x5A);

    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);
    av_packet_free(&tx_pkt);
    av_packet_free(&rx_pkt);

    printf("[PASS] SRT FFmpeg Device Loopback passed.\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("============================================================\n");
    printf("  zff SRT Suite Unit Tests (Subtitle Parser + Transport)\n");
    printf("============================================================\n");

    test_srt_parser_memory();
    test_srt_parser_file();
    test_srt_parser_overlay_integration();
    test_srt_streaming_direct_loopback();
    test_srt_encrypted_loopback();
    test_srt_ffmpeg_device_loopback();

    printf("\nAll SRT unit tests passed successfully!\n");
    _exit(0);
}
