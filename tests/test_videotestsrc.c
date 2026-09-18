/*=============================================================================
    test_videotestsrc.c — Unit tests for zstr_videotestsrc (AVInputFormat)
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "zff/zff_core.h"
#include "zff/plugins/zstr_videotestsrc.h"
#include <libavformat/avformat.h>

static void test_videotestsrc_patterns(void) {
    printf("[TEST] Testing zstr_videotestsrc patterns via standard av_read_frame()...\n");

    const char *patterns[] = { "bars", "gradient", "checkerboard", "noise", "black" };
    for (int i = 0; i < 5; i++) {
        AVFormatContext *fmt_ctx = NULL;
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "video_size", "320x240", 0);
        av_dict_set(&opts, "framerate", "30", 0);
        av_dict_set(&opts, "pattern", patterns[i], 0);
        av_dict_set(&opts, "realtime", "0", 0); /* burst mode for fast testing */
        av_dict_set(&opts, "num_frames", "5", 0);

        const AVInputFormat *iformat = zff_find_input_format("zstr_videotestsrc");
        assert(iformat != NULL);

        int ret = avformat_open_input(&fmt_ctx, "dummy", iformat, &opts);
        assert(ret == 0);
        assert(fmt_ctx != NULL);
        assert(fmt_ctx->nb_streams == 1);
        assert(fmt_ctx->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
        assert(fmt_ctx->streams[0]->codecpar->width == 320);
        assert(fmt_ctx->streams[0]->codecpar->height == 240);

        AVPacket *pkt = av_packet_alloc();
        assert(pkt != NULL);

        int frames_read = 0;
        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            assert(pkt->size == 320 * 240 * 3 / 2);
            assert(pkt->pts == frames_read);
            frames_read++;
            av_packet_unref(pkt);
        }

        assert(frames_read == 5);

        av_packet_free(&pkt);
        avformat_close_input(&fmt_ctx);
        av_dict_free(&opts);
    }
    printf("[PASS] All zstr_videotestsrc patterns passed via standard av_read_frame().\n");
}

static void test_videotestsrc_timing(void) {
    printf("[TEST] Testing zstr_videotestsrc real-time clock pacing...\n");

    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "video_size", "640x360", 0);
    av_dict_set(&opts, "framerate", "20", 0); /* 20 fps = 50ms per frame */
    av_dict_set(&opts, "pattern", "bars", 0);
    av_dict_set(&opts, "realtime", "1", 0);   /* Enforce real-time clock pacing */
    av_dict_set(&opts, "num_frames", "4", 0);

    const AVInputFormat *iformat = zff_find_input_format("zstr_videotestsrc");
    assert(iformat != NULL);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int ret = avformat_open_input(&fmt_ctx, "dummy", iformat, &opts);
    assert(ret == 0);

    AVPacket *pkt = av_packet_alloc();
    int frames = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        frames++;
        av_packet_unref(pkt);
    }
    assert(frames == 4);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
    printf("[INFO] 4 frames at 20fps elapsed: %.2f ms (expected >= 150 ms)\n", elapsed_ms);
    assert(elapsed_ms >= 140.0);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    av_dict_free(&opts);

    printf("[PASS] zstr_videotestsrc real-time clock pacing passed.\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   Running zstr_videotestsrc (AVInputFormat) Tests  \n");
    printf("====================================================\n");

    int ret = zff_plugins_register_all();
    assert(ret == 0);
    (void)ret;

    test_videotestsrc_patterns();
    test_videotestsrc_timing();

    printf("====================================================\n");
    printf("   All zstr_videotestsrc Tests Passed Successfully! \n");
    printf("====================================================\n");
    return 0;
}
