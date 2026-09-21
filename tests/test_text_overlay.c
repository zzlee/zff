/*=============================================================================
    test_text_overlay.c — Unit tests for zstr_text_overlay
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>

#include "zff/plugins/zstr_text_overlay.h"
#include "zff/plugins/zstr_videotestsrc.h"

static AVFrame* create_solid_frame(int w, int h, enum AVPixelFormat fmt, uint8_t y_val, uint8_t u_val, uint8_t v_val)
{
    AVFrame *frame = av_frame_alloc();
    assert(frame != NULL);
    frame->width = w;
    frame->height = h;
    frame->format = fmt;
    int ret = av_frame_get_buffer(frame, 64);
    assert(ret >= 0);

    if (fmt == AV_PIX_FMT_YUV420P) {
        memset(frame->data[0], y_val, frame->linesize[0] * h);
        memset(frame->data[1], u_val, frame->linesize[1] * (h / 2));
        memset(frame->data[2], v_val, frame->linesize[2] * (h / 2));
    } else if (fmt == AV_PIX_FMT_NV12) {
        memset(frame->data[0], y_val, frame->linesize[0] * h);
        memset(frame->data[1], u_val, frame->linesize[1] * (h / 2));
    } else if (fmt == AV_PIX_FMT_RGB24) {
        for (int y = 0; y < h; y++) {
            uint8_t *row = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < w; x++) {
                row[x * 3 + 0] = y_val;
                row[x * 3 + 1] = u_val;
                row[x * 3 + 2] = v_val;
            }
        }
    } else if (fmt == AV_PIX_FMT_RGBA) {
        for (int y = 0; y < h; y++) {
            uint8_t *row = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < w; x++) {
                row[x * 4 + 0] = y_val;
                row[x * 4 + 1] = u_val;
                row[x * 4 + 2] = v_val;
                row[x * 4 + 3] = 255;
            }
        }
    }
    return frame;
}

static void test_lifecycle_and_config(void)
{
    printf("[TEST] Testing text_overlay allocation and config parsing...\n");
    zstr_text_overlay_t *s = zstr_text_overlay_alloc("text=Hello:font_size=28:x=15:y=25:box=1:color=0xFFFF00FF:timecode=0");
    assert(s != NULL);

    assert(zstr_text_overlay_set_param(s, "text=Updated Text:x=30:y=40:timecode=1") == 0);

    zstr_text_overlay_free(&s);
    assert(s == NULL);
    printf("[PASS] text_overlay allocation and config passed.\n");
}

static void test_formats_rendering(void)
{
    printf("[TEST] Testing rendering across pixel formats (YUV420P, NV12, RGB24, RGBA)...\n");
    enum AVPixelFormat fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_RGB24,
        AV_PIX_FMT_RGBA
    };

    for (size_t i = 0; i < sizeof(fmts)/sizeof(fmts[0]); i++) {
        enum AVPixelFormat fmt = fmts[i];
        zstr_text_overlay_t *s = zstr_text_overlay_alloc("text=ZFF:font_size=32:x=10:y=10:box=1:color=0xFFFFFFFF");
        assert(s != NULL);

        /* Create 160x120 solid dark frame */
        AVFrame *in = create_solid_frame(160, 120, fmt, 16, 128, 128);
        in->pts = 1000;
        in->time_base = (AVRational){1, 1000};

        /* Test copy mode (in != out) */
        AVFrame *out = av_frame_alloc();
        int ret = zstr_text_overlay_process(s, in, out);
        assert(ret == 0);
        assert(out->width == 160 && out->height == 120);
        assert(out->format == fmt);
        assert(out->pts == 1000);

        /* Verify some pixels in the text/box area were modified */
        bool modified = false;
        if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_NV12) {
            for (int y = 8; y < 40; y++) {
                for (int x = 8; x < 60; x++) {
                    if (out->data[0][y * out->linesize[0] + x] != 16) {
                        modified = true;
                        break;
                    }
                }
                if (modified) break;
            }
        } else {
            for (int y = 8; y < 40; y++) {
                uint8_t *row = out->data[0] + y * out->linesize[0];
                for (int x = 8; x < 60; x++) {
                    if (row[x * (fmt == AV_PIX_FMT_RGB24 ? 3 : 4)] != 16) {
                        modified = true;
                        break;
                    }
                }
                if (modified) break;
            }
        }
        assert(modified == true);

        /* Test in-place mode (in == out) */
        ret = zstr_text_overlay_process(s, out, out);
        assert(ret == 0);

        av_frame_free(&in);
        av_frame_free(&out);
        zstr_text_overlay_free(&s);
    }
    printf("[PASS] Multi-format rendering passed.\n");
}

static void test_subtitles_and_timecode(void)
{
    printf("[TEST] Testing timed subtitle expiration and timecode...\n");
    zstr_text_overlay_t *s = zstr_text_overlay_alloc("font_size=20:x=5:y=5");
    assert(s != NULL);

    /* Set subtitle valid from PTS 1000 to 2000 ms */
    zstr_text_overlay_set_subtitle(s, "Temporary Subtitle", 1000, 1000, (AVRational){1, 1000});

    AVFrame *f = create_solid_frame(160, 120, AV_PIX_FMT_YUV420P, 0, 128, 128);
    f->time_base = (AVRational){1, 1000};

    /* Frame at PTS 1500 (inside subtitle window) */
    f->pts = 1500;
    int ret = zstr_text_overlay_process(s, f, f);
    assert(ret == 0);

    /* Check that pixel in text region has changed */
    bool text_present = false;
    for (int y = 5; y < 25; y++) {
        for (int x = 5; x < 100; x++) {
            if (f->data[0][y * f->linesize[0] + x] > 0) {
                text_present = true;
                break;
            }
        }
        if (text_present) break;
    }
    assert(text_present == true);

    /* Frame at PTS 2500 (subtitle expired) */
    memset(f->data[0], 0, f->linesize[0] * 120);
    f->pts = 2500;
    ret = zstr_text_overlay_process(s, f, f);
    assert(ret == 0);

    /* Text should not be rendered since subtitle expired and default text is empty */
    text_present = false;
    for (int y = 5; y < 25; y++) {
        for (int x = 5; x < 100; x++) {
            if (f->data[0][y * f->linesize[0] + x] > 0) {
                text_present = true;
                break;
            }
        }
        if (text_present) break;
    }
    assert(text_present == false);

    /* Enable dynamic timecode and verify it renders */
    assert(zstr_text_overlay_set_param(s, "timecode=1") == 0);
    f->pts = 3000;
    ret = zstr_text_overlay_process(s, f, f);
    assert(ret == 0);

    text_present = false;
    for (int y = 5; y < 25; y++) {
        for (int x = 5; x < 120; x++) {
            if (f->data[0][y * f->linesize[0] + x] > 0) {
                text_present = true;
                break;
            }
        }
        if (text_present) break;
    }
    assert(text_present == true);

    av_frame_free(&f);
    zstr_text_overlay_free(&s);
    printf("[PASS] Subtitle expiration and dynamic timecode passed.\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("====================================================\n");
    printf("           Running zstr_text_overlay Tests          \n");
    printf("====================================================\n");

    test_lifecycle_and_config();
    test_formats_rendering();
    test_subtitles_and_timecode();

    printf("====================================================\n");
    printf("    All zstr_text_overlay Tests Passed!             \n");
    printf("====================================================\n");
    return 0;
}
