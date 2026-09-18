/*=============================================================================
    test_v4l2.c — Unit tests for zstr_v4l2 Video Capture Device (AVInputFormat)
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "zff/zff_core.h"
#include "zff/zff_time.h"
#include "zff/plugins/zstr_v4l2.h"
#include <libavformat/avformat.h>

static void test_v4l2_mock_capture(void) {
    printf("[TEST] Testing zstr_v4l2 mock capture via standard av_read_frame()...\n");

    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "video_size", "640x480", 0);
    av_dict_set(&opts, "framerate", "30", 0);
    av_dict_set(&opts, "pixel_format", "yuyv422", 0);
    av_dict_set(&opts, "is_mock", "1", 0);
    av_dict_set(&opts, "realtime", "0", 0); /* burst for fast test */
    av_dict_set(&opts, "num_frames", "5", 0);

    const AVInputFormat *iformat = zff_find_input_format("zstr_v4l2");
    assert(iformat != NULL);

    int ret = avformat_open_input(&fmt_ctx, "v4l2_mock", iformat, &opts);
    assert(ret == 0);
    assert(fmt_ctx != NULL);
    assert(fmt_ctx->nb_streams == 1);
    assert(fmt_ctx->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
    assert(fmt_ctx->streams[0]->codecpar->width == 640);
    assert(fmt_ctx->streams[0]->codecpar->height == 480);
    assert(fmt_ctx->streams[0]->codecpar->format == AV_PIX_FMT_YUYV422);

    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int frames_read = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        assert(pkt->size == 640 * 480 * 2);
        assert(pkt->pts == frames_read);

        /* Verify PTP timestamp is present */
        zff_ptp_time_t ptp = {0};
        int ptp_ret = zff_packet_get_ptp(pkt, &ptp);
        assert(ptp_ret == 0);
        assert(ptp.tai_nanoseconds > 0);

        frames_read++;
        av_packet_unref(pkt);
    }

    assert(frames_read == 5);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    av_dict_free(&opts);
    printf("[PASS] zstr_v4l2 mock capture passed.\n");
}

static void test_v4l2_dmabuf_mode(void) {
    printf("[TEST] Testing zstr_v4l2 mmap-export (DMABUF) mode...\n");

    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "video_size", "320x240", 0);
    av_dict_set(&opts, "framerate", "30", 0);
    av_dict_set(&opts, "pixel_format", "nv12", 0);
    av_dict_set(&opts, "memory_type", "mmap-export", 0);
    av_dict_set(&opts, "is_mock", "1", 0);
    av_dict_set(&opts, "realtime", "0", 0);
    av_dict_set(&opts, "num_frames", "3", 0);

    const AVInputFormat *iformat = zff_find_input_format("zstr_v4l2");
    assert(iformat != NULL);

    int ret = avformat_open_input(&fmt_ctx, "v4l2_dmabuf", iformat, &opts);
    assert(ret == 0);
    assert(fmt_ctx != NULL);
    assert(fmt_ctx->streams[0]->codecpar->format == AV_PIX_FMT_NV12);

    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int frames_read = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        assert(pkt->size == 320 * 240 * 3 / 2);
        frames_read++;
        av_packet_unref(pkt);
    }

    assert(frames_read == 3);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    av_dict_free(&opts);
    printf("[PASS] zstr_v4l2 mmap-export mode passed.\n");
}

int main(void) {
    printf("====================================================\n");
    printf("        Running zstr_v4l2 (AVInputFormat) Tests     \n");
    printf("====================================================\n");

    int ret = zff_plugins_register_all();
    assert(ret == 0);
    (void)ret;

    test_v4l2_mock_capture();
    test_v4l2_dmabuf_mode();

    printf("====================================================\n");
    printf("     All zstr_v4l2 Tests Passed Successfully!       \n");
    printf("====================================================\n");
    return 0;
}
