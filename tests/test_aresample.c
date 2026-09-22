/*=============================================================================
    test_aresample.c — Unit tests for zstr_aresample (with ASRC & Dynamic Formats)
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "zff/plugins/zstr_aresample.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

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
    CHECK(zstr_aresample_process(resampler, in1, out1) == 0);
    assert(out1->sample_rate == 48000);

    /* Step 2: Mid-stream input changes dynamically to 32000 Hz, mono (1 ch) */
    AVFrame *in2 = alloc_test_audio_frame(32000, 1, AV_SAMPLE_FMT_S16, 512, 1024);
    AVFrame *out2 = av_frame_alloc();
    CHECK(zstr_aresample_process(resampler, in2, out2) == 0);
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

        CHECK(zstr_aresample_process(resampler, in, out) == 0);
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

static void test_flush_drain(void) {
    printf("[TEST] Testing flush drains filter delay with continuous PTS...\n");

    zstr_aresample_t *resampler = zstr_aresample_alloc("out_sample_rate=48000:out_channels=2:out_sample_fmt=s16");
    assert(resampler != NULL);

    int64_t pts = 2000000000LL;
    const int64_t step_ns = 1024LL * 1000000000LL / 44100LL; /* 23219954 ns */
    int64_t total = 0;
    int64_t last_pts = 0;
    int last_nb = 0;
    for (int i = 0; i < 5; i++) {
        AVFrame *in = alloc_test_audio_frame(44100, 2, AV_SAMPLE_FMT_S16, 1024, pts);
        AVFrame *out = av_frame_alloc();
        CHECK(zstr_aresample_process(resampler, in, out) == 0);
        CHECK(out->nb_samples > 0);
        total += out->nb_samples;
        last_pts = out->pts;
        last_nb = out->nb_samples;
        av_frame_free(&in);
        av_frame_free(&out);
        pts += step_ns;
    }

    /* Drain until empty; PTS must continue seamlessly */
    int64_t flushed = 0;
    int64_t expect_pts = last_pts + last_nb * 1000000000LL / 48000LL;
    for (int i = 0; i < 8; i++) {
        AVFrame *out = av_frame_alloc();
        CHECK(zstr_aresample_flush(resampler, out) == 0);
        if (out->nb_samples == 0) {
            av_frame_free(&out);
            break;
        }
        { int64_t dd = out->pts - expect_pts; if (dd < 0) dd = -dd; CHECK(dd <= 1); }
        expect_pts += out->nb_samples * 1000000000LL / 48000LL;
        flushed += out->nb_samples;
        av_frame_free(&out);
    }
    CHECK(flushed > 0); /* filter delay actually existed and was recovered */

    /* Grand total matches the exact ratio (delay fully drained) */
    int64_t expect_total = 5LL * 1024 * 48000 / 44100;
    int64_t diff = total + flushed - expect_total;
    if (diff < 0) diff = -diff;
    CHECK(diff <= 4);

    zstr_aresample_free(&resampler);
    printf("[PASS] Flush drain passed (recovered %lld delay samples).\n", (long long)flushed);
}

static void test_pts_precision(void) {
    printf("[TEST] Testing output PTS precision and continuity...\n");

    zstr_aresample_t *resampler = zstr_aresample_alloc("out_sample_rate=48000:out_channels=2:out_sample_fmt=s16");
    assert(resampler != NULL);

    const int64_t anchor = 5000000000LL;
    const int64_t step_ns = 1024LL * 1000000000LL / 44100LL;
    int64_t prev_end = 0;
    for (int i = 0; i < 6; i++) {
        AVFrame *in = alloc_test_audio_frame(44100, 2, AV_SAMPLE_FMT_S16, 1024, anchor + i * step_ns);
        AVFrame *out = av_frame_alloc();
        CHECK(zstr_aresample_process(resampler, in, out) == 0);
        CHECK(out->nb_samples > 0);
        if (i == 0) {
            /* First frame lands within filter-delay distance of the anchor */
            int64_t d = out->pts - anchor;
            if (d < 0) d = -d;
            CHECK(d < 2000000LL); /* < 2ms */
        } else {
            /* continuity up to rounding (av_rescale rounds to nearest) */
            int64_t dd = out->pts - prev_end;
            if (dd < 0) dd = -dd;
            CHECK(dd <= 1);
        }
        prev_end = out->pts + out->nb_samples * 1000000000LL / 48000LL;
        av_frame_free(&in);
        av_frame_free(&out);
    }

    zstr_aresample_free(&resampler);
    printf("[PASS] PTS precision passed.\n");
}

static void test_fractional_rate(void) {
    printf("[TEST] Testing fractional rate override 44100 -> 24000.5 Hz...\n");

    /* numer/denom = 48001/2 -> 24000.5 Hz; swr runs at rounded 24001 Hz,
     * compensation trims the exact ratio. 1000 frames separate compensated
     * output (~557330) from uncompensated (~557353) well beyond noise. */
    zstr_aresample_t *resampler = zstr_aresample_alloc(
        "out_sample_rate=24000:out_channels=1:out_sample_fmt=s16:rate_numer=48001:rate_denom=2");
    assert(resampler != NULL);

    int64_t total = 0;
    int64_t pts = 1000000000LL;
    const int frames = 1000;
    for (int i = 0; i < frames; i++) {
        AVFrame *in = alloc_test_audio_frame(44100, 1, AV_SAMPLE_FMT_S16, 1024, pts);
        AVFrame *out = av_frame_alloc();
        CHECK(zstr_aresample_process(resampler, in, out) == 0);
        total += out->nb_samples;
        av_frame_free(&in);
        av_frame_free(&out);
        pts += 1024LL * 1000000000LL / 44100LL;
    }
    for (int i = 0; i < 8; i++) {
        AVFrame *out = av_frame_alloc();
        CHECK(zstr_aresample_flush(resampler, out) == 0);
        total += out->nb_samples;
        int done = (out->nb_samples == 0);
        av_frame_free(&out);
        if (done) break;
    }

    int64_t expect = (int64_t)frames * 1024 * 48001 / 2 / 44100; /* 557329 */
    int64_t diff = total - expect;
    if (diff < 0) diff = -diff;
    CHECK(diff <= 8);

    zstr_aresample_free(&resampler);
    printf("[PASS] Fractional rate passed (total %lld, expected %lld).\n",
           (long long)total, (long long)expect);
}

static void test_set_param_retarget(void) {
    printf("[TEST] Runtime set_param retargets output rate...\n");

    zstr_aresample_t *resampler = zstr_aresample_alloc("out_sample_rate=48000:out_channels=2:out_sample_fmt=s16");
    assert(resampler != NULL);

    AVFrame *in = alloc_test_audio_frame(44100, 2, AV_SAMPLE_FMT_S16, 1024, 0);
    AVFrame *out = av_frame_alloc();
    assert(zstr_aresample_process(resampler, in, out) == 0);
    assert(out->sample_rate == 48000);
    av_frame_unref(out);

    /* Retarget mid-stream; takes effect on the very next process() */
    assert(zstr_aresample_set_param(resampler, "out_sample_rate=44100") == 0);
    assert(zstr_aresample_set_param(NULL, "out_sample_rate=44100") == AVERROR(EINVAL));
    assert(zstr_aresample_process(resampler, in, out) == 0);
    assert(out->sample_rate == 44100);
    assert(out->ch_layout.nb_channels == 2); /* untouched params persist */

    av_frame_free(&in);
    av_frame_free(&out);
    zstr_aresample_free(&resampler);
    printf("[PASS] set_param retarget passed.\n");
}

int main(void) {
    printf("========================================\n");
    printf("   Running zstr_aresample Unit Tests    \n");
    printf("========================================\n");

    test_resampling_basic();
    test_dynamic_format_change();
    test_asrc_drift_compensation();
    test_flush_drain();
    test_pts_precision();
    test_fractional_rate();
    test_set_param_retarget();

    printf("========================================\n");
    printf("   All zstr_aresample Tests Passed!     \n");
    printf("========================================\n");
    return 0;
}
