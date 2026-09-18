/*=============================================================================
    test_glsink.c — Unit tests for zstr_glsink OpenGL Display Sink Device
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zff/zff_core.h"
#include "zff/plugins/zstr_glsink.h"
#include "zff/plugins/zstr_videotestsrc.h"
#include <libavformat/avformat.h>

static void test_glsink_mock_mode(void) {
    printf("[TEST] Testing zstr_glsink in null/mock mode via standard av_write_frame()...\n");

    const AVOutputFormat *oformat = zff_find_output_format("zstr_glsink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    int ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "display");
    assert(ret == 0);
    assert(out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width      = 640;
    st->codecpar->height     = 480;
    st->codecpar->format     = AV_PIX_FMT_YUV420P;
    st->time_base            = (AVRational){ 1, 30 };

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "is_mock", "1", 0);
    ret = avformat_write_header(out_ctx, &opts);
    assert(ret == 0);

    /* Write 5 frames */
    for (int i = 0; i < 5; i++) {
        AVPacket *pkt = av_packet_alloc();
        assert(pkt != NULL);
        ret = av_new_packet(pkt, 640 * 480 * 3 / 2);
        assert(ret == 0);

        memset(pkt->data, i * 16, pkt->size);
        pkt->pts = i;
        pkt->dts = i;
        pkt->stream_index = 0;

        ret = av_write_frame(out_ctx, pkt);
        assert(ret == 0);

        av_packet_free(&pkt);
    }

    ret = av_write_trailer(out_ctx);
    assert(ret == 0);

    avformat_free_context(out_ctx);
    av_dict_free(&opts);
    printf("[PASS] zstr_glsink mock write passed.\n");
}

static void test_loopback_videotestsrc_to_glsink(void) {
    printf("[TEST] Testing end-to-end loopback: zstr_videotestsrc -> zstr_glsink...\n");

    /* 1. Setup Source */
    AVFormatContext *in_ctx = NULL;
    AVDictionary *in_opts = NULL;
    av_dict_set(&in_opts, "video_size", "320x240", 0);
    av_dict_set(&in_opts, "framerate", "30", 0);
    av_dict_set(&in_opts, "pattern", "gradient", 0);
    av_dict_set(&in_opts, "realtime", "0", 0);
    av_dict_set(&in_opts, "num_frames", "8", 0);

    const AVInputFormat *iformat = zff_find_input_format("zstr_videotestsrc");
    assert(iformat != NULL);
    int ret = avformat_open_input(&in_ctx, "dummy", iformat, &in_opts);
    assert(ret == 0);

    /* 2. Setup Sink */
    const AVOutputFormat *oformat = zff_find_output_format("zstr_glsink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "display");
    assert(ret == 0);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width      = in_ctx->streams[0]->codecpar->width;
    st->codecpar->height     = in_ctx->streams[0]->codecpar->height;
    st->codecpar->format     = in_ctx->streams[0]->codecpar->format;
    st->time_base            = in_ctx->streams[0]->time_base;

    AVDictionary *out_opts = NULL;
    av_dict_set(&out_opts, "is_mock", "1", 0);
    ret = avformat_write_header(out_ctx, &out_opts);
    assert(ret == 0);

    /* 3. Stream frames */
    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int count = 0;
    while (av_read_frame(in_ctx, pkt) >= 0) {
        ret = av_write_frame(out_ctx, pkt);
        assert(ret == 0);
        count++;
        av_packet_unref(pkt);
    }
    assert(count == 8);

    av_packet_free(&pkt);
    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);
    av_dict_free(&in_opts);
    av_dict_free(&out_opts);

    printf("[PASS] End-to-end loopback passed (8 frames rendered).\n");
}

int main(void) {
    printf("====================================================\n");
    printf("     Running zstr_glsink (AVOutputFormat) Tests     \n");
    printf("====================================================\n");

    int ret = zff_plugins_register_all();
    assert(ret == 0);
    (void)ret;

    test_glsink_mock_mode();
    test_loopback_videotestsrc_to_glsink();

    printf("====================================================\n");
    printf("    All zstr_glsink Tests Passed Successfully!      \n");
    printf("====================================================\n");
    return 0;
}
