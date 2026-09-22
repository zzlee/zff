/*=============================================================================
    test_scale.c — Unit tests for zstr_scale Video Scaler & Format Converter
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>

#include "zff/plugins/zstr_scale.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)
#include "zff/zff_core.h"
#include "zff/zff_time.h"

static AVFrame* create_test_frame(int w, int h, enum AVPixelFormat fmt, int64_t pts) {
    AVFrame *frame = av_frame_alloc();
    assert(frame != NULL);
    frame->width = w;
    frame->height = h;
    frame->format = fmt;
    frame->pts = pts;

    int ret = av_frame_get_buffer(frame, 64);
    assert(ret >= 0);

    /* Fill pattern */
    for (int y = 0; y < h; y++) {
        memset(frame->data[0] + y * frame->linesize[0], (y + (int)pts) & 0xFF, w);
    }
    if (frame->data[1]) {
        for (int y = 0; y < h / 2; y++) {
            memset(frame->data[1] + y * frame->linesize[1], 128, frame->linesize[1]);
        }
    }
    if (frame->data[2]) {
        for (int y = 0; y < h / 2; y++) {
            memset(frame->data[2] + y * frame->linesize[2], 128, frame->linesize[2]);
        }
    }
    return frame;
}

static void test_scale_downscale_and_alignment(void) {
    printf("[TEST] Downscaling 1280x720 YUV420P -> 640x360 YUV420P with 64-byte alignment...\n");
    zstr_scale_t *s = zstr_scale_alloc("w=640:h=360:format=yuv420p:flags=bilinear:align=64");
    assert(s != NULL);

    AVFrame *in = create_test_frame(1280, 720, AV_PIX_FMT_YUV420P, 100);
    AVFrame *out = av_frame_alloc();

    int ret = zstr_scale_process(s, in, out);
    assert(ret == 0);
    CHECK(out->width == 640);
    CHECK(out->height == 360);
    CHECK(out->format == AV_PIX_FMT_YUV420P);
    CHECK(out->pts == 100);

    /* Verify 64-byte alignment */
    CHECK(out->linesize[0] % 64 == 0);
    CHECK((uintptr_t)out->data[0] % 64 == 0);

    av_frame_free(&in);
    av_frame_free(&out);
    zstr_scale_free(&s);
    printf("  -> OK\n");
}

static void test_scale_format_conversion(void) {
    printf("[TEST] Format conversion YUV420P -> RGB24...\n");
    zstr_scale_t *s = zstr_scale_alloc("w=320:h=240:format=rgb24");
    assert(s != NULL);

    AVFrame *in = create_test_frame(640, 480, AV_PIX_FMT_YUV420P, 200);
    AVFrame *out = av_frame_alloc();

    int ret = zstr_scale_process(s, in, out);
    assert(ret == 0);
    CHECK(out->width == 320);
    CHECK(out->height == 240);
    CHECK(out->format == AV_PIX_FMT_RGB24);
    CHECK(out->pts == 200);

    av_frame_free(&in);
    av_frame_free(&out);
    zstr_scale_free(&s);
    printf("  -> OK\n");
}

static void test_scale_passthrough(void) {
    printf("[TEST] Zero-copy passthrough when dimensions and format match...\n");
    zstr_scale_t *s = zstr_scale_alloc("w=640:h=480:format=yuv420p");
    assert(s != NULL);

    AVFrame *in = create_test_frame(640, 480, AV_PIX_FMT_YUV420P, 300);
    AVFrame *out = av_frame_alloc();

    int ret = zstr_scale_process(s, in, out);
    assert(ret == 0);
    CHECK(out->width == 640);
    CHECK(out->height == 480);
    CHECK(out->format == AV_PIX_FMT_YUV420P);

    /* Zero-copy check: both frames share the same underlying AVBufferRef */
    assert(out->buf[0] != NULL && in->buf[0] != NULL);
    CHECK(out->buf[0]->buffer == in->buf[0]->buffer);

    av_frame_free(&in);
    av_frame_free(&out);
    zstr_scale_free(&s);
    printf("  -> OK\n");
}

static void test_scale_dynamic_reconfiguration(void) {
    printf("[TEST] Dynamic mid-stream input change (1280x720 YUV420P -> 720x480 NV12)...\n");
    zstr_scale_t *s = zstr_scale_alloc("w=320:h=240:format=yuv420p");
    assert(s != NULL);

    AVFrame *in1 = create_test_frame(1280, 720, AV_PIX_FMT_YUV420P, 1);
    AVFrame *out1 = av_frame_alloc();
    int ret = zstr_scale_process(s, in1, out1);
    assert(ret == 0);
    CHECK(out1->width == 320 && out1->height == 240);

    /* Dynamically change input resolution and format to NV12 */
    AVFrame *in2 = create_test_frame(720, 480, AV_PIX_FMT_NV12, 2);
    AVFrame *out2 = av_frame_alloc();
    ret = zstr_scale_process(s, in2, out2);
    assert(ret == 0);
    CHECK(out2->width == 320 && out2->height == 240);
    CHECK(out2->format == AV_PIX_FMT_YUV420P);
    CHECK(out2->pts == 2);

    av_frame_free(&in1);
    av_frame_free(&out1);
    av_frame_free(&in2);
    av_frame_free(&out2);
    zstr_scale_free(&s);
    printf("  -> OK\n");
}

static void test_scale_side_data_propagation(void) {
    printf("[TEST] FourCC PTP SideData propagation through scaling...\n");
    zstr_scale_t *s = zstr_scale_alloc("w=640:h=360:format=yuv420p");
    assert(s != NULL);

    AVFrame *in = create_test_frame(1280, 720, AV_PIX_FMT_YUV420P, 500);

    /* Attach PTP nanosecond side data to in frame */
    zff_ptp_time_t original_ptp = {
        .tai_nanoseconds = 1726668800123456789ULL,
        .domain = 0
    };
    int ret = zff_frame_set_ptp(in, &original_ptp);
    assert(ret == 0);

    AVFrame *out = av_frame_alloc();
    ret = zstr_scale_process(s, in, out);
    assert(ret == 0);

    /* Verify PTP side data is intact on output frame */
    zff_ptp_time_t extracted_ptp = {0};
    ret = zff_frame_get_ptp(out, &extracted_ptp);
    assert(ret == 0);
    CHECK(extracted_ptp.tai_nanoseconds == original_ptp.tai_nanoseconds);

    av_frame_free(&in);
    av_frame_free(&out);
    zstr_scale_free(&s);
    printf("  -> OK\n");
}

static void test_scale_color_range_preserved(void) {
    printf("[TEST] Full-range YUV420P -> RGB24 keeps levels (no limited remap)...\n");
    zstr_scale_t *s = zstr_scale_alloc("w=64:h=64:format=rgb24");
    assert(s != NULL);

    /* Mid-gray Y=180: full-range -> R~180; misread-as-limited -> R~190 */
    AVFrame *in = av_frame_alloc();
    in->width = 64; in->height = 64; in->format = AV_PIX_FMT_YUV420P;
    in->color_range = AVCOL_RANGE_JPEG;
    CHECK(av_frame_get_buffer(in, 32) >= 0);
    memset(in->data[0], 180, 64 * 64);
    memset(in->data[1], 128, 32 * 32);
    memset(in->data[2], 128, 32 * 32);

    AVFrame *out = av_frame_alloc();
    int ret = zstr_scale_process(s, in, out);
    CHECK(ret == 0);
    CHECK(out->format == AV_PIX_FMT_RGB24);
    int r = out->data[0][3000];
    CHECK(r >= 175 && r <= 185);

    /* Limited-range input is unaffected (R~190 expected) */
    AVFrame *in2 = av_frame_alloc();
    in2->width = 64; in2->height = 64; in2->format = AV_PIX_FMT_YUV420P;
    in2->color_range = AVCOL_RANGE_MPEG;
    CHECK(av_frame_get_buffer(in2, 32) >= 0);
    memset(in2->data[0], 180, 64 * 64);
    memset(in2->data[1], 128, 32 * 32);
    memset(in2->data[2], 128, 32 * 32);
    AVFrame *out2 = av_frame_alloc();
    CHECK(zstr_scale_process(s, in2, out2) == 0);
    int r2 = out2->data[0][3000];
    CHECK(r2 >= 185 && r2 <= 195);

    av_frame_free(&in);
    av_frame_free(&out);
    av_frame_free(&in2);
    av_frame_free(&out2);
    zstr_scale_free(&s);
    printf("  -> OK (full=%d limited=%d)\n", r, r2);
}

static void test_scale_set_param(void) {
    printf("[TEST] Runtime set_param retargets output geometry...\n");
    zstr_scale_t *s = zstr_scale_alloc("w=320:h=240:format=yuv420p");
    assert(s != NULL);

    AVFrame *in = create_test_frame(640, 480, AV_PIX_FMT_YUV420P, 0);
    AVFrame *out = av_frame_alloc();
    assert(zstr_scale_process(s, in, out) == 0);
    CHECK(out->width == 320 && out->height == 240);
    av_frame_unref(out);

    /* Retarget mid-stream; takes effect on the very next process() */
    assert(zstr_scale_set_param(s, "w=160:h=120") == 0);
    assert(zstr_scale_set_param(NULL, "w=160") == AVERROR(EINVAL));
    assert(zstr_scale_process(s, in, out) == 0);
    CHECK(out->width == 160 && out->height == 120);
    CHECK(out->format == AV_PIX_FMT_YUV420P); /* untouched params persist */

    av_frame_free(&in);
    av_frame_free(&out);
    zstr_scale_free(&s);
    printf("  -> OK\n");
}

int main(void) {
    printf("=== Starting zstr_scale Unit Tests ===\n");
    test_scale_downscale_and_alignment();
    test_scale_format_conversion();
    test_scale_passthrough();
    test_scale_dynamic_reconfiguration();
    test_scale_side_data_propagation();
    test_scale_color_range_preserved();
    test_scale_set_param();
    printf("=== All zstr_scale Tests PASSED ===\n");
    return 0;
}
