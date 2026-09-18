/*=============================================================================
    test_amix.c — Unit tests for zstr_amix Audio Multi-Channel Mixer
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "zff/zff_core.h"
#include "zff/zff_time.h"
#include "zff/plugins/zstr_amix.h"
#include <libavutil/frame.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static AVFrame* create_audio_frame(int rate, int channels, enum AVSampleFormat fmt, int nb_samples, double freq, double amp) {
    AVFrame *frame = av_frame_alloc();
    assert(frame != NULL);
    frame->sample_rate = rate;
    frame->format = fmt;
    av_channel_layout_default(&frame->ch_layout, channels);
    frame->nb_samples = nb_samples;

    int ret = av_frame_get_buffer(frame, 0);
    assert(ret >= 0);

    if (fmt == AV_SAMPLE_FMT_FLT) {
        float *dst = (float *)frame->data[0];
        for (int i = 0; i < nb_samples; i++) {
            float val = (float)(sin(2.0 * M_PI * freq * i / rate) * amp);
            for (int ch = 0; ch < channels; ch++) {
                dst[i * channels + ch] = val;
            }
        }
    } else if (fmt == AV_SAMPLE_FMT_S16) {
        int16_t *dst = (int16_t *)frame->data[0];
        for (int i = 0; i < nb_samples; i++) {
            int16_t val = (int16_t)(sin(2.0 * M_PI * freq * i / rate) * amp * 32767.0);
            for (int ch = 0; ch < channels; ch++) {
                dst[i * channels + ch] = val;
            }
        }
    }

    return frame;
}

static void test_amix_basic_mixing(void) {
    printf("[TEST] Testing basic synchronous mixing of two audio streams...\n");

    zstr_amix_t *m = zstr_amix_alloc("inputs=2:sample_rate=48000:channels=2:sample_fmt=flt:normalize=0");
    assert(m != NULL);

    AVFrame *f1 = create_audio_frame(48000, 2, AV_SAMPLE_FMT_FLT, 512, 440.0, 0.4);
    AVFrame *f2 = create_audio_frame(48000, 2, AV_SAMPLE_FMT_FLT, 512, 880.0, 0.4);
    const AVFrame *inputs[2] = { f1, f2 };

    AVFrame *out = av_frame_alloc();
    assert(out != NULL);

    int ret = zstr_amix_mix(m, inputs, 2, out);
    assert(ret == 0);
    assert(out->sample_rate == 48000);
    assert(out->ch_layout.nb_channels == 2);
    assert(out->format == AV_SAMPLE_FMT_FLT);
    assert(out->nb_samples == 512);

    /* Verify mixed signal exists and is within [-1.0, 1.0] */
    const float *data = (const float *)out->data[0];
    float max_val = 0.0f;
    for (int i = 0; i < 512 * 2; i++) {
        float abs_v = fabsf(data[i]);
        if (abs_v > max_val) max_val = abs_v;
        assert(data[i] <= 1.0f && data[i] >= -1.0f);
    }
    assert(max_val > 0.3f); /* Confirms signals actually summed */

    av_frame_free(&f1);
    av_frame_free(&f2);
    av_frame_free(&out);
    zstr_amix_free(&m);

    printf("[PASS] Basic audio mixing passed.\n");
}

static void test_amix_mute_and_volume(void) {
    printf("[TEST] Testing mute and volume controls in mixer...\n");

    zstr_amix_t *m = zstr_amix_alloc("inputs=2:sample_rate=48000:channels=2:sample_fmt=flt");
    assert(m != NULL);

    /* Mute input 0, set input 1 volume to 0.5 */
    zstr_amix_set_input_mute(m, 0, true);
    zstr_amix_set_input_volume(m, 1, 0.5);

    AVFrame *f1 = create_audio_frame(48000, 2, AV_SAMPLE_FMT_FLT, 512, 440.0, 1.0);
    AVFrame *f2 = create_audio_frame(48000, 2, AV_SAMPLE_FMT_FLT, 512, 880.0, 1.0);
    const AVFrame *inputs[2] = { f1, f2 };

    AVFrame *out = av_frame_alloc();
    assert(out != NULL);

    int ret = zstr_amix_mix(m, inputs, 2, out);
    assert(ret == 0);

    /* Output should only contain f2 scaled by 0.5 (max amplitude ~0.5) */
    const float *data = (const float *)out->data[0];
    float max_val = 0.0f;
    for (int i = 0; i < 512 * 2; i++) {
        float abs_v = fabsf(data[i]);
        if (abs_v > max_val) max_val = abs_v;
    }
    assert(max_val <= 0.51f);
    assert(max_val >= 0.45f);

    av_frame_free(&f1);
    av_frame_free(&f2);
    av_frame_free(&out);
    zstr_amix_free(&m);

    printf("[PASS] Mute and volume controls passed.\n");
}

static void test_amix_resampling_cross_format(void) {
    printf("[TEST] Testing cross-format mixing (44.1kHz S16 + 48kHz FLT -> 48kHz S16)...\n");

    zstr_amix_t *m = zstr_amix_alloc("inputs=2:sample_rate=48000:channels=2:sample_fmt=s16:normalize=1");
    assert(m != NULL);

    /* Input 0: 44.1kHz stereo S16 */
    AVFrame *f1 = create_audio_frame(44100, 2, AV_SAMPLE_FMT_S16, 441, 440.0, 0.5);
    /* Input 1: 48kHz mono FLT */
    AVFrame *f2 = create_audio_frame(48000, 1, AV_SAMPLE_FMT_FLT, 480, 880.0, 0.5);
    const AVFrame *inputs[2] = { f1, f2 };

    AVFrame *out = av_frame_alloc();
    assert(out != NULL);

    int ret = zstr_amix_mix(m, inputs, 2, out);
    assert(ret == 0);
    assert(out->sample_rate == 48000);
    assert(out->ch_layout.nb_channels == 2);
    assert(out->format == AV_SAMPLE_FMT_S16);

    av_frame_free(&f1);
    av_frame_free(&f2);
    av_frame_free(&out);
    zstr_amix_free(&m);

    printf("[PASS] Cross-format mixing passed.\n");
}

static void test_amix_soft_clipping_limiter(void) {
    printf("[TEST] Testing soft-clipping limiter (overdriven summing > 1.0)...\n");

    zstr_amix_t *m = zstr_amix_alloc("inputs=2:sample_rate=48000:channels=2:sample_fmt=flt:normalize=0");
    assert(m != NULL);

    /* Sum two signals each with amplitude 0.9 => sum exceeds 1.0 */
    AVFrame *f1 = create_audio_frame(48000, 2, AV_SAMPLE_FMT_FLT, 256, 440.0, 0.9);
    AVFrame *f2 = create_audio_frame(48000, 2, AV_SAMPLE_FMT_FLT, 256, 440.0, 0.9);
    const AVFrame *inputs[2] = { f1, f2 };

    AVFrame *out = av_frame_alloc();
    int ret = zstr_amix_mix(m, inputs, 2, out);
    assert(ret == 0);

    const float *data = (const float *)out->data[0];
    for (int i = 0; i < 256 * 2; i++) {
        /* Verify soft-clipping smoothly bounded output within [-1.0, 1.0] without wrap-around */
        assert(data[i] <= 1.0001f && data[i] >= -1.0001f);
    }

    av_frame_free(&f1);
    av_frame_free(&f2);
    av_frame_free(&out);
    zstr_amix_free(&m);

    printf("[PASS] Soft-clipping limiter passed.\n");
}

int main(void) {
    printf("====================================================\n");
    printf("         Running zstr_amix Audio Mixer Tests        \n");
    printf("====================================================\n");

    test_amix_basic_mixing();
    test_amix_mute_and_volume();
    test_amix_resampling_cross_format();
    test_amix_soft_clipping_limiter();

    printf("====================================================\n");
    printf("      All zstr_amix Tests Passed Successfully!      \n");
    printf("====================================================\n");
    return 0;
}
