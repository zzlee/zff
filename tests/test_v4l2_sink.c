/*=============================================================================
    test_v4l2_sink.c — Unit tests for zstr_v4l2_sink Video Output Device
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zff/zff_core.h"
#include "zff/plugins/zstr_v4l2_sink.h"
#include "zff/plugins/zstr_videotestsrc.h"
#include <libavformat/avformat.h>

static void test_v4l2_sink_basic(void) {
    printf("[TEST] Testing zstr_v4l2_sink output device via standard av_write_frame()...\n");

    const AVOutputFormat *oformat = zff_find_output_format("zstr_v4l2_sink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    int ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "dummy");
    assert(ret == 0);
    assert(out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width      = 640;
    st->codecpar->height     = 480;
    st->codecpar->format     = AV_PIX_FMT_YUYV422;
    st->time_base            = (AVRational){ 1, 30 };

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "is_mock", "1", 0);
    ret = avformat_write_header(out_ctx, &opts);
    assert(ret == 0);

    /* Write 5 frames */
    for (int i = 0; i < 5; i++) {
        AVPacket *pkt = av_packet_alloc();
        assert(pkt != NULL);
        ret = av_new_packet(pkt, 640 * 480 * 2);
        assert(ret == 0);

        memset(pkt->data, i * 20, pkt->size);
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
    printf("[PASS] zstr_v4l2_sink basic write passed.\n");
}

static void test_loopback_videotestsrc_to_v4l2_sink(void) {
    printf("[TEST] Testing end-to-end loopback: zstr_videotestsrc -> zstr_v4l2_sink...\n");

    /* 1. Setup Source */
    AVFormatContext *in_ctx = NULL;
    AVDictionary *in_opts = NULL;
    av_dict_set(&in_opts, "video_size", "320x240", 0);
    av_dict_set(&in_opts, "framerate", "30", 0);
    av_dict_set(&in_opts, "pattern", "bars", 0);
    av_dict_set(&in_opts, "realtime", "0", 0);
    av_dict_set(&in_opts, "num_frames", "10", 0);

    const AVInputFormat *iformat = zff_find_input_format("zstr_videotestsrc");
    assert(iformat != NULL);
    int ret = avformat_open_input(&in_ctx, "dummy", iformat, &in_opts);
    assert(ret == 0);

    /* 2. Setup Sink */
    const AVOutputFormat *oformat = zff_find_output_format("zstr_v4l2_sink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "dummy");
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

    /* 3. Stream frames from source into sink */
    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int count = 0;
    while (av_read_frame(in_ctx, pkt) >= 0) {
        ret = av_write_frame(out_ctx, pkt);
        assert(ret == 0);
        count++;
        av_packet_unref(pkt);
    }
    assert(count == 10);

    av_packet_free(&pkt);
    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);
    av_dict_free(&in_opts);
    av_dict_free(&out_opts);

    printf("[PASS] End-to-end loopback passed (10 frames processed).\n");
}

int main(void) {
    printf("====================================================\n");
    printf("     Running zstr_v4l2_sink (AVOutputFormat) Tests  \n");
    printf("====================================================\n");

    int ret = zff_plugins_register_all();
    assert(ret == 0);
    (void)ret;

    test_v4l2_sink_basic();
    test_loopback_videotestsrc_to_v4l2_sink();

    printf("====================================================\n");
    printf("   All zstr_v4l2_sink Tests Passed Successfully!    \n");
    printf("====================================================\n");
    return 0;
}
