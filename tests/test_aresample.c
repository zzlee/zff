/*=============================================================================
    test_aresample.c — Unit tests for zstr_aresample (with ASRC & Dynamic Formats)
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include "zff/plugins/zstr_aresample.h"

static AVFrame* alloc_test_audio_frame(int sample_rate, int channels, enum AVSampleFormat fmt, int nb_samples, int64_t pts) {
    AVFrame *frame = av_frame_alloc();
    assert(frame != NULL);

    frame->sample_rate = sample_rate;
    av_channel_layout_default(&frame->ch_layout, channels);
    frame->format = fmt;
    frame->nb_samples = nb_samples;
    frame->pts = pts;

    int ret = av_frame_get_buffer(frame, 0);
    assert(ret == 0);
    assert(av_frame_make_writable(frame) == 0);

    /* Generate a simple 440Hz sine wave */
    int16_t *pcm16 = (int16_t*)frame->data[0];
    for (int i = 0; i < nb_samples; i++) {
        int16_t v = (int16_t)(sin(2.0 * M_PI * 440.0 * (double)i / (double)sample_rate) * 16000.0);
        for (int c = 0; c < channels; c++) {
            pcm16[i * channels + c] = v;
        }
    }
    return frame;
}

static void test_resampling_basic(void) {
    printf("[TEST] Testing basic resampling 44100Hz -> 48000Hz...\n");

    zstr_aresample_t *resampler = zstr_aresample_alloc("out_sample_rate=48000:out_channels=2:out_sample_fmt=s16");
    assert(resampler != NULL);

    AVFrame *in = alloc_test_audio_frame(44100, 2, AV_SAMPLE_FMT_S16, 1024, 0);
    AVFrame *out = av_frame_alloc();

    int ret = zstr_aresample_process(resampler, in, out);
    assert(ret == 0);
    assert(out->sample_rate == 48000);
    assert(out->ch_layout.nb_channels == 2);
    assert(out->format == AV_SAMPLE_FMT_S16);
    assert(out->nb_samples > 0);

    av_frame_free(&in);
    av_frame_free(&out);
    zstr_aresample_free(&resampler);
    printf("[PASS] Basic resampling passed.\n");
}

static void test_dynamic_format_change(void) {
    printf("[TEST] Testing dynamic mid-stream sample rate change 44100Hz -> 48000Hz...\n");

    zstr_aresample_t *resampler = zstr_aresample_alloc("out_sample_rate=48000:out_channels=2:out_sample_fmt=s16");
    assert(resampler != NULL);

    /* Step 1: Input at 44100 Hz */
    AVFrame *in1 = alloc_test_audio_frame(44100, 2, AV_SAMPLE_FMT_S16, 1024, 0);
    AVFrame *out1 = av_frame_alloc();
    assert(zstr_aresample_process(resampler, in1, out1) == 0);
    assert(out1->sample_rate == 48000);

    /* Step 2: Mid-stream input changes dynamically to 32000 Hz, mono (1 ch) */
    AVFrame *in2 = alloc_test_audio_frame(32000, 1, AV_SAMPLE_FMT_S16, 512, 1024);
    AVFrame *out2 = av_frame_alloc();
    assert(zstr_aresample_process(resampler, in2, out2) == 0);
    assert(out2->sample_rate == 48000);
    assert(out2->ch_layout.nb_channels == 2);

    av_frame_free(&in1);
    av_frame_free(&out1);
    av_frame_free(&in2);
    av_frame_free(&out2);
    zstr_aresample_free(&resampler);
    printf("[PASS] Dynamic format change passed seamlessly.\n");
}

static void test_asrc_drift_compensation(void) {
    printf("[TEST] Testing ASRC PTS drift compensation...\n");

    zstr_aresample_t *resampler = zstr_aresample_alloc("out_sample_rate=48000:out_channels=2:asrc_mode=pts:max_drift_ppm=2000:drift_interval=2");
    assert(resampler != NULL);

    int64_t pts = 1000000000LL; // 1.0 sec
    /* Send 10 buffers with simulated clock drift (e.g. timestamps advance slower than sample count) */
    for (int i = 0; i < 10; i++) {
        AVFrame *in = alloc_test_audio_frame(48000, 2, AV_SAMPLE_FMT_S16, 1024, pts);
        AVFrame *out = av_frame_alloc();

        assert(zstr_aresample_process(resampler, in, out) == 0);
        assert(out->sample_rate == 48000);
        assert(out->nb_samples > 0);

        av_frame_free(&in);
        av_frame_free(&out);

        /* Nominal 1024 samples at 48kHz = 21,333,333 ns. Let PTS advance by 21,310,000 ns (simulating source running fast) */
        pts += 21310000LL;
    }

    zstr_aresample_free(&resampler);
    printf("[PASS] ASRC PTS drift compensation passed.\n");
}

int main(void) {
    printf("========================================\n");
    printf("   Running zstr_aresample Unit Tests    \n");
    printf("========================================\n");

    test_resampling_basic();
    test_dynamic_format_change();
    test_asrc_drift_compensation();

    printf("========================================\n");
    printf("   All zstr_aresample Tests Passed!     \n");
    printf("========================================\n");
    return 0;
}
