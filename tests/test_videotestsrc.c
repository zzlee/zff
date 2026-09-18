/*=============================================================================
    test_videotestsrc.c — Unit tests for zstr_videotestsrc
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zff/plugins/zstr_videotestsrc.h"
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

static void test_patterns(void) {
    printf("[TEST] Testing all video patterns...\n");

    const char *patterns[] = { "bars", "gradient", "checkerboard", "noise", "black" };
    for (int i = 0; i < 5; i++) {
        char opts[128];
        snprintf(opts, sizeof(opts), "w=320:h=240:pattern=%s:num_frames=3", patterns[i]);
        zstr_videotestsrc_t *src = zstr_videotestsrc_alloc(opts);
        assert(src != NULL);
        assert(src->width == 320);
        assert(src->height == 240);

        AVFrame *frame = av_frame_alloc();
        assert(frame != NULL);

        for (int f = 0; f < 3; f++) {
            int ret = zstr_videotestsrc_read_frame(src, frame);
            assert(ret == 0);
            assert(frame->width == 320);
            assert(frame->height == 240);
            assert(frame->pts == f);
            assert(frame->format == AV_PIX_FMT_YUV420P);
            assert(frame->data[0] != NULL);
            assert(frame->data[1] != NULL);
            assert(frame->data[2] != NULL);
            av_frame_unref(frame);
        }

        /* 4th read must return EOF */
        int eof_ret = zstr_videotestsrc_read_frame(src, frame);
        assert(eof_ret == AVERROR_EOF);

        av_frame_free(&frame);
        zstr_videotestsrc_free(&src);
        assert(src == NULL);
    }
    printf("[PASS] All video patterns passed.\n");
}

static void test_filtergraph_integration(void) {
    printf("[TEST] Testing videotestsrc FilterGraph integration...\n");

    zstr_videotestsrc_t *src = zstr_videotestsrc_alloc("w=640:h=360:pattern=bars:rate=30");
    assert(src != NULL);

    AVFilterGraph *graph = avfilter_graph_alloc();
    assert(graph != NULL);

    AVFilterContext *src_ctx = NULL;
    assert(zstr_videotestsrc_attach_to_graph(src, graph, &src_ctx) == 0);
    assert(src_ctx != NULL);

    /* Create buffersink */
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    assert(buffersink != NULL);
    AVFilterContext *sink_ctx = NULL;
    assert(avfilter_graph_create_filter(&sink_ctx, buffersink, "sink", NULL, NULL, graph) == 0);

    /* Link src -> sink */
    assert(avfilter_link(src_ctx, 0, sink_ctx, 0) == 0);
    assert(avfilter_graph_config(graph, NULL) == 0);

    /* Generate frame and push into graph */
    AVFrame *frame = av_frame_alloc();
    assert(zstr_videotestsrc_read_frame(src, frame) == 0);
    assert(av_buffersrc_add_frame(src_ctx, frame) == 0);

    /* Retrieve from sink */
    AVFrame *out_frame = av_frame_alloc();
    assert(av_buffersink_get_frame(sink_ctx, out_frame) == 0);
    assert(out_frame->width == 640);
    assert(out_frame->height == 360);

    av_frame_free(&frame);
    av_frame_free(&out_frame);
    avfilter_graph_free(&graph);
    zstr_videotestsrc_free(&src);

    printf("[PASS] Video FilterGraph integration passed.\n");
}

int main(void) {
    printf("========================================\n");
    printf("   Running zstr_videotestsrc Tests\n");
    printf("========================================\n");

    test_patterns();
    test_filtergraph_integration();

    printf("========================================\n");
    printf("   All videotestsrc Tests Passed!\n");
    printf("========================================\n");
    return 0;
}
