/*=============================================================================
    test_audiotestsrc.c — Unit tests for zstr_audiotestsrc (AVInputFormat)
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "zff/zff_core.h"
#include "zff/plugins/zstr_audiotestsrc.h"
#include <libavformat/avformat.h>

static void test_audiotestsrc_waves(void) {
    printf("[TEST] Testing zstr_audiotestsrc waveforms via standard av_read_frame()...\n");

    const char *waves[] = { "sine", "square", "white_noise", "pink_noise", "silence" };
    for (int i = 0; i < 5; i++) {
        AVFormatContext *fmt_ctx = NULL;
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "sample_rate", "44100", 0);
        av_dict_set(&opts, "channels", "2", 0);
        av_dict_set(&opts, "wave", waves[i], 0);
        av_dict_set(&opts, "samples_per_frame", "512", 0);
        av_dict_set(&opts, "realtime", "0", 0); /* burst mode */
        av_dict_set(&opts, "num_samples", "1536", 0); /* exactly 3 packets of 512 */

        const AVInputFormat *iformat = zff_find_input_format("zstr_audiotestsrc");
        assert(iformat != NULL);

        int ret = avformat_open_input(&fmt_ctx, "dummy", iformat, &opts);
        assert(ret == 0);
        assert(fmt_ctx != NULL);
        assert(fmt_ctx->nb_streams == 1);
        assert(fmt_ctx->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO);
        assert(fmt_ctx->streams[0]->codecpar->sample_rate == 44100);
        assert(fmt_ctx->streams[0]->codecpar->ch_layout.nb_channels == 2);

        AVPacket *pkt = av_packet_alloc();
        assert(pkt != NULL);

        int packets_read = 0;
        int64_t expected_pts = 0;
        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            assert(pkt->size == 512 * 2 * sizeof(int16_t));
            assert(pkt->pts == expected_pts);
            expected_pts += 512;
            packets_read++;
            av_packet_unref(pkt);
        }

        assert(packets_read == 3);

        av_packet_free(&pkt);
        avformat_close_input(&fmt_ctx);
        av_dict_free(&opts);
    }
    printf("[PASS] All zstr_audiotestsrc waveforms passed via standard av_read_frame().\n");
}

static void test_audiotestsrc_cadence(void) {
    printf("[TEST] Testing zstr_audiotestsrc real-time audio cadence pacing...\n");

    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "sample_rate", "48000", 0);
    av_dict_set(&opts, "channels", "2", 0);
    av_dict_set(&opts, "wave", "sine", 0);
    av_dict_set(&opts, "samples_per_frame", "1024", 0); /* 1024 / 48000 = ~21.33 ms per frame */
    av_dict_set(&opts, "realtime", "1", 0);            /* Real-time audio pacing */
    av_dict_set(&opts, "num_samples", "4096", 0);       /* 4 buffers = ~85.3 ms */

    const AVInputFormat *iformat = zff_find_input_format("zstr_audiotestsrc");
    assert(iformat != NULL);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int ret = avformat_open_input(&fmt_ctx, "dummy", iformat, &opts);
    assert(ret == 0);

    AVPacket *pkt = av_packet_alloc();
    int packets = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        packets++;
        av_packet_unref(pkt);
    }
    assert(packets == 4);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
    printf("[INFO] 4 audio packets (4096 samples at 48kHz) elapsed: %.2f ms (expected >= 60 ms)\n", elapsed_ms);
    assert(elapsed_ms >= 55.0);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    av_dict_free(&opts);

    printf("[PASS] zstr_audiotestsrc real-time audio cadence pacing passed.\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   Running zstr_audiotestsrc (AVInputFormat) Tests  \n");
    printf("====================================================\n");

    assert(zff_plugins_register_all() == 0);

    test_audiotestsrc_waves();
    test_audiotestsrc_cadence();

    printf("====================================================\n");
    printf("   All zstr_audiotestsrc Tests Passed Successfully! \n");
    printf("====================================================\n");
    return 0;
}
