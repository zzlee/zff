/*=============================================================================
    test_v4l2.c — Unit tests for zstr_v4l2 Video Capture Device (AVInputFormat)
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "zff/zff_core.h"
#include "zff/zff_time.h"
#include "zff/zff_hw.h"
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

static void test_v4l2_pixel_format_matrix(void) {
    printf("[TEST] Testing zstr_v4l2 mock capture across pixel formats...\n");

    struct { const char *name; int fmt; int size; } cases[] = {
        { "yuyv422", AV_PIX_FMT_YUYV422, 320 * 240 * 2 },
        { "yuv420p", AV_PIX_FMT_YUV420P, 320 * 240 * 3 / 2 },
        { "nv12",    AV_PIX_FMT_NV12,    320 * 240 * 3 / 2 },
        { "nv16",    AV_PIX_FMT_NV16,    320 * 240 * 2 },
        { "rgb24",   AV_PIX_FMT_RGB24,   320 * 240 * 3 },
        { "bgr24",   AV_PIX_FMT_BGR24,   320 * 240 * 3 },
        { "rgb32",   AV_PIX_FMT_0RGB,    320 * 240 * 4 },
        { "bgr32",   AV_PIX_FMT_BGR0,    320 * 240 * 4 },
    };

    const AVInputFormat *iformat = zff_find_input_format("zstr_v4l2");
    assert(iformat != NULL);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        AVFormatContext *fmt_ctx = NULL;
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "video_size", "320x240", 0);
        av_dict_set(&opts, "framerate", "30", 0);
        av_dict_set(&opts, "pixel_format", cases[i].name, 0);
        av_dict_set(&opts, "is_mock", "1", 0);
        av_dict_set(&opts, "realtime", "0", 0);
        av_dict_set(&opts, "num_frames", "2", 0);

        int ret = avformat_open_input(&fmt_ctx, "v4l2_mock", iformat, &opts);
        assert(ret == 0);
        assert(fmt_ctx->streams[0]->codecpar->format == cases[i].fmt);

        AVPacket *pkt = av_packet_alloc();
        int frames = 0;
        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            assert(pkt->size == cases[i].size);
            frames++;
            av_packet_unref(pkt);
        }
        assert(frames == 2);
        av_packet_free(&pkt);
        avformat_close_input(&fmt_ctx);
        av_dict_free(&opts);
    }
    printf("[PASS] Pixel format matrix passed (9 formats).\n");
}

static void test_v4l2_dmabuf_import_mode(void) {
    printf("[TEST] Testing zstr_v4l2 DMABUF import mode (zero-copy side data)...\n");

    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "video_size", "320x240", 0);
    av_dict_set(&opts, "framerate", "30", 0);
    av_dict_set(&opts, "pixel_format", "nv12", 0);
    av_dict_set(&opts, "memory_type", "dmabuf", 0);   /* V4L2_MEMORY_DMABUF */
    av_dict_set(&opts, "is_mock", "1", 0);            /* memfd-backed pool    */
    av_dict_set(&opts, "realtime", "0", 0);
    av_dict_set(&opts, "num_frames", "3", 0);

    const AVInputFormat *iformat = zff_find_input_format("zstr_v4l2");
    assert(iformat != NULL);

    int ret = avformat_open_input(&fmt_ctx, "v4l2_dmabuf_import", iformat, &opts);
    assert(ret == 0);
    assert(fmt_ctx != NULL);
    assert(fmt_ctx->streams[0]->codecpar->format == AV_PIX_FMT_NV12);

    int memfds_seen[16];
    int nb_fds = 0;

    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int frames_read = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        /* Zero-copy mode: no payload copy — the fd rides via side data. */
        assert(pkt->size == 0);

        ZffDMABufInfo info = {0};
        assert(zff_packet_get_dmabuf(pkt, &info) == 0);
        assert(info.fd >= 0);
        assert(info.width == 320 && info.height == 240);
        assert(info.drm_format == ZFF_DRM_FORMAT_NV12);
        assert(info.y_offset == 0 && info.y_pitch == 320);
        assert(info.uv_offset == 320 * 240 && info.uv_pitch == 320);
        assert(info.size == 320 * 240 * 3 / 2);
        assert(info.bytesused == 320 * 240 * 3 / 2);

        /* The same fd must keep cycling through the pool. */
        assert(nb_fds < (int)(sizeof(memfds_seen) / sizeof(memfds_seen[0])));
        int dup = 0;
        for (int i = 0; i < nb_fds; i++) {
            if (memfds_seen[i] == info.fd) { dup = 1; break; }
        }
        if (!dup && nb_fds < 4) memfds_seen[nb_fds++] = info.fd;

        /* PTP still attached alongside DMABUF info. */
        zff_ptp_time_t ptp = {0};
        assert(zff_packet_get_ptp(pkt, &ptp) == 0);
        assert(ptp.tai_nanoseconds > 0);

        frames_read++;
        av_packet_unref(pkt);
    }

    assert(frames_read == 3);
    assert(nb_fds == 3); /* frames 0..2 cycle through pool indices 0,1,2 */

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    av_dict_free(&opts);
    printf("[PASS] zstr_v4l2 DMABUF import mode passed (zero-copy side data).\n");
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
    test_v4l2_dmabuf_import_mode();
    test_v4l2_pixel_format_matrix();

    printf("====================================================\n");
    printf("     All zstr_v4l2 Tests Passed Successfully!       \n");
    printf("====================================================\n");
    return 0;
}
