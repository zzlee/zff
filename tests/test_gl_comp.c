/*=============================================================================
    test_gl_comp.c — Unit tests for zstr_gl_comp OpenGL Video Compositor
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavcodec/packet.h>

#include "zff/plugins/zstr_gl_comp.h"
#include "zff/plugins/zstr_gl_comp_engine.h"
#include "zff/zff_core.h"

static AVFrame* create_solid_frame(int w, int h, enum AVPixelFormat fmt, uint8_t r, uint8_t g, uint8_t b)
{
    AVFrame *frame = av_frame_alloc();
    assert(frame != NULL);
    frame->width = w;
    frame->height = h;
    frame->format = fmt;
    int ret = av_frame_get_buffer(frame, 64);
    assert(ret >= 0);

    if (fmt == AV_PIX_FMT_RGB24) {
        for (int y = 0; y < h; y++) {
            uint8_t *row = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < w; x++) {
                row[x * 3 + 0] = r;
                row[x * 3 + 1] = g;
                row[x * 3 + 2] = b;
            }
        }
    } else if (fmt == AV_PIX_FMT_YUV420P) {
        /* Approximate RGB to YUV */
        uint8_t y_val = (uint8_t)((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        uint8_t u_val = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        uint8_t v_val = (uint8_t)((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

        memset(frame->data[0], y_val, frame->linesize[0] * h);
        memset(frame->data[1], u_val, frame->linesize[1] * (h / 2));
        memset(frame->data[2], v_val, frame->linesize[2] * (h / 2));
    }
    return frame;
}

static void test_compositor_lifecycle_and_layout(void)
{
    printf("[TEST] Testing compositor lifecycle and multi-layer layout...\n");
    zstr_gl_comp_t *c = zstr_gl_comp_alloc("w=640:h=480:bg=0x000000FF:mock=1");
    assert(c != NULL);

    /* Configure side-by-side layout */
    zstr_gl_comp_layer_cfg_t l0 = {
        .x = 0, .y = 120, .width = 320, .height = 240,
        .z_order = 0, .alpha = 1.0f, .visible = true,
        .border_width = 0, .border_color = 0
    };
    zstr_gl_comp_layer_cfg_t l1 = {
        .x = 320, .y = 120, .width = 320, .height = 240,
        .z_order = 1, .alpha = 1.0f, .visible = true,
        .border_width = 2, .border_color = 0xFFFFFFFF
    };

    assert(zstr_gl_comp_configure_layer(c, 0, &l0) == 0);
    assert(zstr_gl_comp_configure_layer(c, 1, &l1) == 0);

    /* Create test frames: Layer 0 Red (RGB24), Layer 1 Green (YUV420P) */
    AVFrame *f0 = create_solid_frame(160, 120, AV_PIX_FMT_RGB24, 255, 0, 0);
    AVFrame *f1 = create_solid_frame(160, 120, AV_PIX_FMT_YUV420P, 0, 255, 0);

    assert(zstr_gl_comp_set_layer_frame(c, 0, f0) == 0);
    assert(zstr_gl_comp_set_layer_frame(c, 1, f1) == 0);

    /* Render composition */
    assert(zstr_gl_comp_render(c) == 0);

    /* Capture output canvas */
    AVFrame *captured = av_frame_alloc();
    captured->format = AV_PIX_FMT_RGBA;
    captured->width = 640;
    captured->height = 480;
    assert(zstr_gl_comp_capture(c, captured) == 0);

    /* Verify Layer 0 region (x=160, y=240) has high Red */
    uint8_t *p_l0 = captured->data[0] + 240 * captured->linesize[0] + 160 * 4;
    assert(p_l0[0] > 200); /* Red component */
    assert(p_l0[1] < 50);  /* Green component */

    /* Verify Layer 1 region (x=480, y=240) has high Green */
    uint8_t *p_l1 = captured->data[0] + 240 * captured->linesize[0] + 480 * 4;
    assert(p_l1[0] < 50);  /* Red component */
    assert(p_l1[1] > 180); /* Green component */

    /* Verify background area (x=10, y=10) is black */
    uint8_t *p_bg = captured->data[0] + 10 * captured->linesize[0] + 10 * 4;
    assert(p_bg[0] == 0 && p_bg[1] == 0 && p_bg[2] == 0);

    av_frame_free(&f0);
    av_frame_free(&f1);
    av_frame_free(&captured);
    zstr_gl_comp_free(&c);
    assert(c == NULL);

    printf("[PASS] Compositor lifecycle and multi-layer layout passed.\n");
}

static void test_pip_alpha_and_border(void)
{
    printf("[TEST] Testing Picture-in-Picture (PiP) with alpha and border...\n");
    zstr_gl_comp_t *c = zstr_gl_comp_alloc("w=640:h=480:bg=0x101010FF:mock=1");
    assert(c != NULL);

    /* Layer 0: Fullscreen background (Blue) */
    zstr_gl_comp_layer_cfg_t l_bg = {
        .x = 0, .y = 0, .width = 640, .height = 480,
        .z_order = 0, .alpha = 1.0f, .visible = true
    };
    /* Layer 1: Inset PiP window (Red) with 50% opacity and 4px yellow border */
    zstr_gl_comp_layer_cfg_t l_pip = {
        .x = 400, .y = 300, .width = 200, .height = 150,
        .z_order = 10, .alpha = 0.5f, .visible = true,
        .border_width = 4, .border_color = 0xFFFF00FF
    };

    zstr_gl_comp_configure_layer(c, 0, &l_bg);
    zstr_gl_comp_configure_layer(c, 1, &l_pip);

    AVFrame *f_blue = create_solid_frame(320, 240, AV_PIX_FMT_RGB24, 0, 0, 255);
    AVFrame *f_red = create_solid_frame(160, 120, AV_PIX_FMT_RGB24, 255, 0, 0);

    zstr_gl_comp_set_layer_frame(c, 0, f_blue);
    zstr_gl_comp_set_layer_frame(c, 1, f_red);

    assert(zstr_gl_comp_render(c) == 0);

    AVFrame *out = av_frame_alloc();
    out->format = AV_PIX_FMT_RGBA;
    out->width = 640;
    out->height = 480;
    assert(zstr_gl_comp_capture(c, out) == 0);

    /* Check PiP blended pixel at center of PiP (x=500, y=375): should have both red and blue */
    uint8_t *p_blend = out->data[0] + 375 * out->linesize[0] + 500 * 4;
    assert(p_blend[0] > 100); /* Red from PiP */
    assert(p_blend[2] > 100); /* Blue from background */

    /* Check border pixel at (x=398, y=320): should be yellow (Red high, Green high) */
    uint8_t *p_border = out->data[0] + 320 * out->linesize[0] + 398 * 4;
    assert(p_border[0] > 200); /* Red */
    assert(p_border[1] > 200); /* Green */

    av_frame_free(&f_blue);
    av_frame_free(&f_red);
    av_frame_free(&out);
    zstr_gl_comp_free(&c);

    printf("[PASS] PiP alpha blending and border passed.\n");
}

static void test_outdev_muxer(void)
{
    printf("[TEST] Testing zstr_gl_comp outdev (AVOutputFormat) with 2 streams...\n");

    AVFormatContext *oc = NULL;
    int ret = avformat_alloc_output_context2(&oc, &ff_zstr_gl_comp_muxer.p, "zstr_gl_comp", NULL);
    assert(ret >= 0 && oc != NULL);

    /* Add Stream 0 */
    AVStream *st0 = avformat_new_stream(oc, NULL);
    assert(st0 != NULL);
    st0->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st0->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    st0->codecpar->format = AV_PIX_FMT_RGB24;
    st0->codecpar->width = 320;
    st0->codecpar->height = 240;

    /* Add Stream 1 */
    AVStream *st1 = avformat_new_stream(oc, NULL);
    assert(st1 != NULL);
    st1->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st1->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    st1->codecpar->format = AV_PIX_FMT_RGB24;
    st1->codecpar->width = 320;
    st1->codecpar->height = 240;

    /* Force mock mode */
    av_opt_set_int(oc->priv_data, "is_mock", 1, 0);

    ret = avformat_write_header(oc, NULL);
    assert(ret >= 0);

    /* Send packets to Stream 0 and Stream 1 */
    int frame_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, 320, 240, 1);
    uint8_t *dummy_buf0 = malloc(frame_bytes);
    uint8_t *dummy_buf1 = malloc(frame_bytes);
    memset(dummy_buf0, 0x55, frame_bytes);
    memset(dummy_buf1, 0xAA, frame_bytes);

    AVPacket *pkt0 = av_packet_alloc();
    AVPacket *pkt1 = av_packet_alloc();

    for (int i = 0; i < 5; i++) {
        pkt0->stream_index = 0;
        pkt0->data = dummy_buf0;
        pkt0->size = frame_bytes;
        pkt0->pts = i * 1000;
        assert(av_write_frame(oc, pkt0) == 0);

        pkt1->stream_index = 1;
        pkt1->data = dummy_buf1;
        pkt1->size = frame_bytes;
        pkt1->pts = i * 1000;
        assert(av_write_frame(oc, pkt1) == 0);
    }

    av_packet_free(&pkt0);
    av_packet_free(&pkt1);
    free(dummy_buf0);
    free(dummy_buf1);

    av_write_trailer(oc);
    avformat_free_context(oc);

    printf("[PASS] zstr_gl_comp outdev muxer passed.\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    zff_plugins_register_all();

    printf("====================================================\n");
    printf("        Running zstr_gl_comp Compositor Tests       \n");
    printf("====================================================\n");

    test_compositor_lifecycle_and_layout();
    test_pip_alpha_and_border();
    test_outdev_muxer();

    printf("====================================================\n");
    printf("    All zstr_gl_comp Tests Passed Successfully!     \n");
    printf("====================================================\n");
    return 0;
}
