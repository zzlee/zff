/*=============================================================================
    test_alsa_sink.c — Unit tests for zstr_alsa_sink Audio Output Device
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zff/zff_core.h"
#include "zff/plugins/zstr_alsa_sink.h"
#include "zff/plugins/zstr_audiotestsrc.h"
#include <libavformat/avformat.h>

static void test_alsa_sink_basic(void) {
    printf("[TEST] Testing zstr_alsa_sink output device via standard av_write_frame()...\n");

    const AVOutputFormat *oformat = zff_find_output_format("zstr_alsa_sink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    int ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "dummy");
    assert(ret == 0);
    assert(out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = AV_CODEC_ID_PCM_S16LE;
    st->codecpar->sample_rate = 48000;
    av_channel_layout_default(&st->codecpar->ch_layout, 2);
    st->codecpar->format     = AV_SAMPLE_FMT_S16;
    st->time_base            = (AVRational){ 1, 48000 };

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "is_mock", "1", 0);
    ret = avformat_write_header(out_ctx, &opts);
    assert(ret == 0);

    /* Write 5 audio packets of 512 samples */
    int block_bytes = 512 * 2 * (int)sizeof(int16_t);
    for (int i = 0; i < 5; i++) {
        AVPacket *pkt = av_packet_alloc();
        assert(pkt != NULL);
        ret = av_new_packet(pkt, block_bytes);
        assert(ret == 0);

        memset(pkt->data, i & 0xFF, block_bytes);
        pkt->pts = i * 512;
        pkt->dts = i * 512;
        pkt->stream_index = 0;

        ret = av_write_frame(out_ctx, pkt);
        assert(ret == 0);

        av_packet_free(&pkt);
    }

    ret = av_write_trailer(out_ctx);
    assert(ret == 0);

    avformat_free_context(out_ctx);
    av_dict_free(&opts);
    printf("[PASS] zstr_alsa_sink basic write passed.\n");
}

static void test_loopback_audiotestsrc_to_alsa_sink(void) {
    printf("[TEST] Testing end-to-end audio loopback: zstr_audiotestsrc -> zstr_alsa_sink...\n");

    /* 1. Setup Source */
    AVFormatContext *in_ctx = NULL;
    AVDictionary *in_opts = NULL;
    av_dict_set(&in_opts, "sample_rate", "44100", 0);
    av_dict_set(&in_opts, "channels", "2", 0);
    av_dict_set(&in_opts, "wave", "sine", 0);
    av_dict_set(&in_opts, "samples_per_frame", "512", 0);
    av_dict_set(&in_opts, "realtime", "0", 0);
    av_dict_set(&in_opts, "num_samples", "2560", 0); /* 5 packets */

    const AVInputFormat *iformat = zff_find_input_format("zstr_audiotestsrc");
    assert(iformat != NULL);
    int ret = avformat_open_input(&in_ctx, "dummy", iformat, &in_opts);
    assert(ret == 0);

    /* 2. Setup Sink */
    const AVOutputFormat *oformat = zff_find_output_format("zstr_alsa_sink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "dummy");
    assert(ret == 0);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = in_ctx->streams[0]->codecpar->codec_id;
    st->codecpar->sample_rate = in_ctx->streams[0]->codecpar->sample_rate;
    av_channel_layout_copy(&st->codecpar->ch_layout, &in_ctx->streams[0]->codecpar->ch_layout);
    st->codecpar->format     = in_ctx->streams[0]->codecpar->format;
    st->time_base            = in_ctx->streams[0]->time_base;

    AVDictionary *out_opts = NULL;
    av_dict_set(&out_opts, "is_mock", "1", 0);
    ret = avformat_write_header(out_ctx, &out_opts);
    assert(ret == 0);

    /* 3. Stream packets */
    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int count = 0;
    while (av_read_frame(in_ctx, pkt) >= 0) {
        ret = av_write_frame(out_ctx, pkt);
        assert(ret == 0);
        count++;
        av_packet_unref(pkt);
    }
    assert(count == 5);

    av_packet_free(&pkt);
    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);
    av_dict_free(&in_opts);
    av_dict_free(&out_opts);

    printf("[PASS] End-to-end audio loopback passed (5 packets processed).\n");
}

int main(void) {
    printf("====================================================\n");
    printf("     Running zstr_alsa_sink (AVOutputFormat) Tests  \n");
    printf("====================================================\n");

    int ret = zff_plugins_register_all();
    assert(ret == 0);
    (void)ret;

    test_alsa_sink_basic();
    test_loopback_audiotestsrc_to_alsa_sink();

    printf("====================================================\n");
    printf("   All zstr_alsa_sink Tests Passed Successfully!    \n");
    printf("====================================================\n");
    return 0;
}
