/*=============================================================================
    test_alsa.c — Unit tests for zstr_alsa Audio Capture Device (AVInputFormat)
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "zff/zff_core.h"
#include "zff/zff_time.h"
#include "zff/plugins/zstr_alsa.h"
#include <libavformat/avformat.h>

static void test_alsa_mock_s16le(void) {
    printf("[TEST] Testing zstr_alsa mock capture (48kHz, stereo, s16)...\n");

    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "sample_rate", "48000", 0);
    av_dict_set(&opts, "channels", "2", 0);
    av_dict_set(&opts, "sample_fmt", "s16", 0);
    av_dict_set(&opts, "block_size", "512", 0);
    av_dict_set(&opts, "is_mock", "1", 0);
    av_dict_set(&opts, "realtime", "0", 0); /* burst */
    av_dict_set(&opts, "num_samples", "2048", 0); /* exactly 4 packets */

    const AVInputFormat *iformat = zff_find_input_format("zstr_alsa");
    assert(iformat != NULL);

    int ret = avformat_open_input(&fmt_ctx, "alsa_mock", iformat, &opts);
    assert(ret == 0);
    assert(fmt_ctx != NULL);
    assert(fmt_ctx->nb_streams == 1);
    assert(fmt_ctx->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO);
    assert(fmt_ctx->streams[0]->codecpar->sample_rate == 48000);
    assert(fmt_ctx->streams[0]->codecpar->ch_layout.nb_channels == 2);
    assert(fmt_ctx->streams[0]->codecpar->format == AV_SAMPLE_FMT_S16);

    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int packets_read = 0;
    int64_t expected_pts = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        assert(pkt->size == 512 * 2 * (int)sizeof(int16_t));
        assert(pkt->pts == expected_pts);

        /* Verify FourCC PTP metadata is attached */
        zff_ptp_time_t ptp = {0};
        int ptp_ret = zff_packet_get_ptp(pkt, &ptp);
        assert(ptp_ret == 0);
        assert(ptp.tai_nanoseconds > 0);

        expected_pts += 512;
        packets_read++;
        av_packet_unref(pkt);
    }

    assert(packets_read == 4);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    av_dict_free(&opts);
    printf("[PASS] zstr_alsa mock capture s16le passed.\n");
}

static void test_alsa_mock_float(void) {
    printf("[TEST] Testing zstr_alsa mock capture (96kHz, mono, flt)...\n");

    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "sample_rate", "96000", 0);
    av_dict_set(&opts, "channels", "1", 0);
    av_dict_set(&opts, "sample_fmt", "flt", 0);
    av_dict_set(&opts, "block_size", "1024", 0);
    av_dict_set(&opts, "is_mock", "1", 0);
    av_dict_set(&opts, "realtime", "0", 0);
    av_dict_set(&opts, "num_samples", "2048", 0);

    const AVInputFormat *iformat = zff_find_input_format("zstr_alsa");
    assert(iformat != NULL);

    int ret = avformat_open_input(&fmt_ctx, "alsa_float", iformat, &opts);
    assert(ret == 0);
    assert(fmt_ctx != NULL);
    assert(fmt_ctx->streams[0]->codecpar->format == AV_SAMPLE_FMT_FLT);

    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int packets_read = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        assert(pkt->size == 1024 * 1 * (int)sizeof(float));
        packets_read++;
        av_packet_unref(pkt);
    }

    assert(packets_read == 2);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    av_dict_free(&opts);
    printf("[PASS] zstr_alsa mock capture float passed.\n");
}

int main(void) {
    printf("====================================================\n");
    printf("        Running zstr_alsa (AVInputFormat) Tests     \n");
    printf("====================================================\n");

    int ret = zff_plugins_register_all();
    assert(ret == 0);
    (void)ret;

    test_alsa_mock_s16le();
    test_alsa_mock_float();

    printf("====================================================\n");
    printf("     All zstr_alsa Tests Passed Successfully!       \n");
    printf("====================================================\n");
    return 0;
}
