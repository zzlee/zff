/*=============================================================================
    test_hwaccel_vaapi.c — VAAPI hardware video path integration test

    Exercises the native oneAPI-adjacent HW video stack this machine
    provides (Intel Quick Sync silicon via iHD + libva): device creation,
    HW frame upload/download, h264_vaapi encode, and VAAPI decode, all
    through public libav* API that zff plugins build on.

    NOTE on scope: Intel's VPL GPU runtime (libmfxhw64) is not packaged
    for this distro snapshot, so the MFX/VPL dispatcher has no HW
    implementation (qsv session creation fails with MFX_ERR_UNSUPPORTED).
    Until Intel ships vpl-gpu-rt here, VAAPI validates the same silicon
    and driver. A qsv/VPL variant of this test slots in unchanged once
    the runtime exists.

    Requires /dev/dri at RUNTIME (docker: --device=/dev/dri:/dev/dri
    --group-add <video> --group-add <render>). Without it the test
    SKIPs (CI runners have no GPU).
 =============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "zff/zff_core.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

#define W 320
#define H 240
#define NFRAMES 30

static AVBufferRef *g_dev = NULL;

static void fill_gradient(AVFrame *f, int idx)
{
    /* NV12: plane 0 = Y (W x H), plane 1 = interleaved UV (W bytes x H/2) */
    for (int y = 0; y < f->height; y++) {
        memset(f->data[0] + y * f->linesize[0], (uint8_t)((y * 255) / f->height), f->width);
    }
    for (int y = 0; y < f->height / 2; y++) {
        memset(f->data[1] + y * f->linesize[1], 128, f->width);
    }
    (void)idx;
}

static int try_open_device(void)
{
    /* renderD128 first (headless), card0 as fallback */
    if (av_hwdevice_ctx_create(&g_dev, AV_HWDEVICE_TYPE_VAAPI,
                               "/dev/dri/renderD128", NULL, 0) == 0)
        return 0;
    if (av_hwdevice_ctx_create(&g_dev, AV_HWDEVICE_TYPE_VAAPI,
                               "/dev/dri/card0", NULL, 0) == 0)
        return 0;
    return -1;
}

static AVBufferRef *make_frames_ctx(void)
{
    AVBufferRef *frames = av_hwframe_ctx_alloc(g_dev);
    CHECK(frames != NULL);
    AVHWFramesContext *fc = (AVHWFramesContext *)frames->data;
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = W;
    fc->height = H;
    CHECK(av_hwframe_ctx_init(frames) == 0);
    return frames;
}

static void test_hwframe_xfer(void)
{
    printf("[TEST] VAAPI HWFrames upload/download roundtrip...\n");
    AVBufferRef *frames = make_frames_ctx();

    AVFrame *sw = av_frame_alloc();
    sw->format = AV_PIX_FMT_NV12;
    sw->width = W;
    sw->height = H;
    CHECK(av_frame_get_buffer(sw, 0) == 0);
    CHECK(av_frame_make_writable(sw) == 0);
    fill_gradient(sw, 0);

    AVFrame *hw = av_frame_alloc();
    CHECK(av_hwframe_get_buffer(frames, hw, 0) == 0);
    CHECK(av_hwframe_transfer_data(hw, sw, 0) == 0);

    AVFrame *back = av_frame_alloc();
    back->format = AV_PIX_FMT_NV12;
    back->width = W;
    back->height = H;
    CHECK(av_frame_get_buffer(back, 0) == 0);
    CHECK(av_hwframe_transfer_data(back, hw, 0) == 0);

    /* NV12 upload/download is bit-exact on iHD */
    int diff = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (sw->data[0][y * sw->linesize[0] + x] !=
                back->data[0][y * back->linesize[0] + x])
                diff++;
    printf("[INFO] Y-plane differing bytes: %d / %d\n", diff, W * H);
    CHECK(diff == 0);

    av_frame_free(&sw);
    av_frame_free(&hw);
    av_frame_free(&back);
    av_buffer_unref(&frames);
    printf("[PASS] HWFrames xfer passed.\n");
}

/* Encoded packets shared with the decode tests */
static AVPacket **g_h264_pkts = NULL;
static int g_h264_nb = 0;
static AVPacket **g_hevc_pkts = NULL;
static int g_hevc_nb = 0;

/* H.264: first packet must carry an IDR slice (NAL type 5) */
static int has_h264_idr(const AVPacket *pkt)
{
    for (int i = 0; i + 4 < pkt->size; i++) {
        if (pkt->data[i] == 0 && pkt->data[i+1] == 0 &&
            pkt->data[i+2] == 0 && pkt->data[i+3] == 1 &&
            (pkt->data[i+4] & 0x1F) == 5) return 1;
        if (pkt->data[i] == 0 && pkt->data[i+1] == 0 &&
            pkt->data[i+2] == 1 &&
            (pkt->data[i+3] & 0x1F) == 5) return 1;
    }
    return 0;
}

/* HEVC: NAL type lives in bits 1..6 of the first header byte;
 * 19 = IDR_W_RADL, 20 = IDR_N_LP, 21 = CRA */
static int has_hevc_idr(const AVPacket *pkt)
{
    for (int i = 0; i + 4 < pkt->size; i++) {
        int off = -1;
        if (pkt->data[i] == 0 && pkt->data[i+1] == 0 &&
            pkt->data[i+2] == 0 && pkt->data[i+3] == 1) off = i + 4;
        else if (pkt->data[i] == 0 && pkt->data[i+1] == 0 &&
                 pkt->data[i+2] == 1) off = i + 3;
        if (off > 0) {
            int t = (pkt->data[off] >> 1) & 0x3F;
            if (t == 19 || t == 20 || t == 21) return 1;
        }
    }
    return 0;
}

static void encode_codec(const char *enc_name, AVPacket ***out_pkts, int *out_nb)
{
    const AVCodec *enc = avcodec_find_encoder_by_name(enc_name);
    CHECK(enc != NULL);

    AVBufferRef *frames = make_frames_ctx();
    AVCodecContext *ec = avcodec_alloc_context3(enc);
    CHECK(ec != NULL);
    ec->width = W;
    ec->height = H;
    ec->time_base = (AVRational){ 1, 30 };
    ec->framerate = (AVRational){ 30, 1 };
    ec->pix_fmt = AV_PIX_FMT_VAAPI;
    ec->hw_frames_ctx = av_buffer_ref(frames);
    ec->bit_rate = 2000000;
    ec->gop_size = 30;
    ec->max_b_frames = 0;
    CHECK(avcodec_open2(ec, enc, NULL) == 0);

    AVFrame *sw = av_frame_alloc();
    sw->format = AV_PIX_FMT_NV12;
    sw->width = W;
    sw->height = H;
    CHECK(av_frame_get_buffer(sw, 0) == 0);

    size_t total_bytes = 0;
    AVPacket **pkts = calloc(NFRAMES * 2, sizeof(AVPacket *));
    CHECK(pkts != NULL);
    int nb = 0;

    for (int i = 0; i < NFRAMES; i++) {
        CHECK(av_frame_make_writable(sw) == 0);
        fill_gradient(sw, i);
        sw->pts = i;
        AVFrame *hw = av_frame_alloc();
        CHECK(av_hwframe_get_buffer(frames, hw, 0) == 0);
        CHECK(av_hwframe_transfer_data(hw, sw, 0) == 0);
        hw->pts = i;
        CHECK(avcodec_send_frame(ec, hw) == 0);
        av_frame_free(&hw);
        AVPacket *pkt = av_packet_alloc();
        while (avcodec_receive_packet(ec, pkt) == 0) {
            total_bytes += pkt->size;
            pkts[nb++] = pkt;
            pkt = av_packet_alloc();
        }
        av_packet_free(&pkt);
    }
    CHECK(avcodec_send_frame(ec, NULL) == 0);
    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(ec, pkt) == 0) {
        total_bytes += pkt->size;
        pkts[nb++] = pkt;
        pkt = av_packet_alloc();
    }
    av_packet_free(&pkt);

    printf("[INFO] %s: %d packets, %zu bytes.\n", enc_name, nb, total_bytes);
    CHECK(nb > 0 && total_bytes > 500);
    if (!strcmp(enc_name, "h264_vaapi"))
        CHECK(has_h264_idr(pkts[0]));
    else
        CHECK(has_hevc_idr(pkts[0]));

    av_frame_free(&sw);
    avcodec_free_context(&ec);
    av_buffer_unref(&frames);
    *out_pkts = pkts;
    *out_nb = nb;
}

static void test_vaapi_encode(void)
{
    printf("[TEST] h264_vaapi encode %d frames...\n", NFRAMES);
    encode_codec("h264_vaapi", &g_h264_pkts, &g_h264_nb);
    printf("[PASS] VAAPI encode passed.\n");
}

/* HEVC (H.265) encode path: same shape, IDR is NAL types 19/20/21 */
static void test_hevc_encode(void)
{
    printf("[TEST] hevc_vaapi encode %d frames...\n", NFRAMES);
    encode_codec("hevc_vaapi", &g_hevc_pkts, &g_hevc_nb);
    printf("[PASS] HEVC encode passed.\n");
}

static enum AVPixelFormat vaapi_get_format(AVCodecContext *ctx,
                                           const enum AVPixelFormat *pix_fmts)
{
    (void)ctx;
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    }
    return AV_PIX_FMT_NONE;
}

static void decode_codec(enum AVCodecID codec_id, const char *label,
                         AVPacket **pkts, int nb_pkts)
{
    printf("[TEST] VAAPI decode %s packets...\n", label);
    const AVCodec *dec = avcodec_find_decoder(codec_id);
    CHECK(dec != NULL);
    AVCodecContext *dc = avcodec_alloc_context3(dec);
    CHECK(dc != NULL);
    dc->get_format = vaapi_get_format;
    AVBufferRef *hw_dev = NULL;
    CHECK(av_hwdevice_ctx_create(&hw_dev, AV_HWDEVICE_TYPE_VAAPI,
                                 "/dev/dri/renderD128", NULL, 0) == 0 ||
          av_hwdevice_ctx_create(&hw_dev, AV_HWDEVICE_TYPE_VAAPI,
                                 "/dev/dri/card0", NULL, 0) == 0);
    dc->hw_device_ctx = av_buffer_ref(hw_dev);
    CHECK(avcodec_open2(dc, dec, NULL) == 0);

    int got_frames = 0;
    int saw_hw = 0;
    long y_sum = 0;
    AVFrame *f = av_frame_alloc();
    for (int i = 0; i < nb_pkts; i++) {
        CHECK(avcodec_send_packet(dc, pkts[i]) == 0);
        while (avcodec_receive_frame(dc, f) == 0) {
            got_frames++;
            if (f->format == AV_PIX_FMT_VAAPI) {
                saw_hw = 1;
                AVFrame *sw = av_frame_alloc();
                sw->format = AV_PIX_FMT_NV12;
                if (av_hwframe_transfer_data(sw, f, 0) == 0) {
                    for (int y = 0; y < H; y += 7)
                        y_sum += sw->data[0][y * sw->linesize[0] + W / 2];
                }
                av_frame_free(&sw);
            }
            av_frame_unref(f);
        }
    }
    CHECK(avcodec_send_packet(dc, NULL) == 0);
    while (avcodec_receive_frame(dc, f) == 0) {
        got_frames++;
        av_frame_unref(f);
    }
    printf("[INFO] %s decoded %d frames (hw=%d, y_sum=%ld).\n",
           label, got_frames, saw_hw, y_sum);
    CHECK(got_frames >= NFRAMES - 2);
    CHECK(saw_hw);
    CHECK(y_sum > 0); /* gradient content survived the roundtrip */

    av_frame_free(&f);
    avcodec_free_context(&dc);
    av_buffer_unref(&hw_dev);
}

static void test_vaapi_decode(void)
{
    decode_codec(AV_CODEC_ID_H264, "H264", g_h264_pkts, g_h264_nb);
    printf("[PASS] VAAPI decode passed.\n");
}

static void test_hevc_decode(void)
{
    decode_codec(AV_CODEC_ID_HEVC, "HEVC", g_hevc_pkts, g_hevc_nb);
    printf("[PASS] HEVC decode passed.\n");
}

int main(void)
{
    printf("====================================================\n");
    printf("        Running VAAPI HW Integration Tests          \n");
    printf("====================================================\n");

    if (try_open_device() < 0) {
        printf("[SKIP] No VAAPI device (/dev/dri); skipping HW tests.\n");
        printf("====================================================\n");
        printf("       VAAPI tests skipped (no GPU).                \n");
        printf("====================================================\n");
        return 0;
    }
    printf("[INFO] VAAPI device opened.\n");

    test_hwframe_xfer();
    test_vaapi_encode();
    test_vaapi_decode();
    test_hevc_encode();
    test_hevc_decode();

    for (int i = 0; i < g_h264_nb; i++) av_packet_free(&g_h264_pkts[i]);
    free(g_h264_pkts);
    for (int i = 0; i < g_hevc_nb; i++) av_packet_free(&g_hevc_pkts[i]);
    free(g_hevc_pkts);
    av_buffer_unref(&g_dev);

    printf("====================================================\n");
    printf("       All VAAPI HW Tests Passed!                   \n");
    printf("====================================================\n");
    return 0;
}
