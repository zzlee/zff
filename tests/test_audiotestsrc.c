/*=============================================================================
    test_audiotestsrc.c — Unit tests for zstr_audiotestsrc
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zff/plugins/zstr_audiotestsrc.h"
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

static void test_waves(void) {
    printf("[TEST] Testing all audio waveforms...\n");

    const char *waves[] = { "sine", "square", "white_noise", "pink_noise", "silence" };
    for (int i = 0; i < 5; i++) {
        char opts[128];
        snprintf(opts, sizeof(opts), "r=44100:c=2:wave=%s:samples_per_frame=512:num_samples=1536", waves[i]);
        zstr_audiotestsrc_t *src = zstr_audiotestsrc_alloc(opts);
        assert(src != NULL);
        assert(src->sample_rate == 44100);
        assert(src->channels == 2);

        AVFrame *frame = av_frame_alloc();
        assert(frame != NULL);

        for (int f = 0; f < 3; f++) {
            int ret = zstr_audiotestsrc_read_frame(src, frame);
            assert(ret == 0);
            assert(frame->nb_samples == 512);
            assert(frame->sample_rate == 44100);
            assert(frame->pts == f * 512);
            assert(frame->data[0] != NULL);
            av_frame_unref(frame);
        }

        /* 4th read must return EOF because num_samples = 1536 (3 * 512) */
        int eof_ret = zstr_audiotestsrc_read_frame(src, frame);
        assert(eof_ret == AVERROR_EOF);

        av_frame_free(&frame);
        zstr_audiotestsrc_free(&src);
        assert(src == NULL);
    }
    printf("[PASS] All audio waveforms passed.\n");
}

static void test_filtergraph_integration(void) {
    printf("[TEST] Testing audiotestsrc FilterGraph integration...\n");

    zstr_audiotestsrc_t *src = zstr_audiotestsrc_alloc("r=48000:c=2:wave=sine:f=1000:samples_per_frame=1024");
    assert(src != NULL);

    AVFilterGraph *graph = avfilter_graph_alloc();
    assert(graph != NULL);

    AVFilterContext *src_ctx = NULL;
    assert(zstr_audiotestsrc_attach_to_graph(src, graph, &src_ctx) == 0);
    assert(src_ctx != NULL);

    /* Create abuffersink */
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    assert(abuffersink != NULL);
    AVFilterContext *sink_ctx = NULL;
    assert(avfilter_graph_create_filter(&sink_ctx, abuffersink, "sink", NULL, NULL, graph) == 0);

    /* Link src -> sink */
    assert(avfilter_link(src_ctx, 0, sink_ctx, 0) == 0);
    assert(avfilter_graph_config(graph, NULL) == 0);

    /* Generate frame and push into graph */
    AVFrame *frame = av_frame_alloc();
    assert(zstr_audiotestsrc_read_frame(src, frame) == 0);
    assert(av_buffersrc_add_frame(src_ctx, frame) == 0);

    /* Retrieve from sink */
    AVFrame *out_frame = av_frame_alloc();
    assert(av_buffersink_get_frame(sink_ctx, out_frame) == 0);
    assert(out_frame->nb_samples == 1024);
    assert(out_frame->sample_rate == 48000);

    av_frame_free(&frame);
    av_frame_free(&out_frame);
    avfilter_graph_free(&graph);
    zstr_audiotestsrc_free(&src);

    printf("[PASS] Audio FilterGraph integration passed.\n");
}

int main(void) {
    printf("========================================\n");
    printf("   Running zstr_audiotestsrc Tests\n");
    printf("========================================\n");

    test_waves();
    test_filtergraph_integration();

    printf("========================================\n");
    printf("   All audiotestsrc Tests Passed!\n");
    printf("========================================\n");
    return 0;
}
