/*=============================================================================
    test_x11sink.c — Unit tests for zstr_x11sink X11 Display Sink Device
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "zff/zff_core.h"
#include "zff/plugins/zstr_x11sink.h"
#include "zff/plugins/zstr_videotestsrc.h"
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <X11/Xlib.h>

static void test_x11sink_mock_mode(void) {
    printf("[TEST] Testing zstr_x11sink in null/mock mode via standard av_write_frame()...\n");

    const AVOutputFormat *oformat = zff_find_output_format("zstr_x11sink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    int ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "display");
    assert(ret == 0);
    assert(out_ctx != NULL);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width      = 640;
    st->codecpar->height     = 480;
    st->codecpar->format     = AV_PIX_FMT_YUV420P;
    st->time_base            = (AVRational){ 1, 30 };

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "is_mock", "1", 0);
    ret = avformat_write_header(out_ctx, &opts);
    assert(ret == 0);

    for (int i = 0; i < 5; i++) {
        AVPacket *pkt = av_packet_alloc();
        assert(pkt != NULL);
        ret = av_new_packet(pkt, 640 * 480 * 3 / 2);
        assert(ret == 0);

        memset(pkt->data, i * 16, pkt->size);
        pkt->pts = i;
        pkt->dts = i;
        pkt->stream_index = 0;

        ret = av_write_frame(out_ctx, pkt);
        assert(ret == 0);

        av_packet_free(&pkt);
    }

    ret = av_write_trailer(out_ctx);
    assert(ret == 0);

    avformat_free_context(out_ctx);
    av_dict_free(&opts);
    printf("[PASS] zstr_x11sink mock write passed.\n");
}

static void test_loopback_videotestsrc_to_x11sink(void) {
    printf("[TEST] Testing end-to-end loopback: zstr_videotestsrc -> zstr_x11sink...\n");

    AVFormatContext *in_ctx = NULL;
    AVDictionary *in_opts = NULL;
    av_dict_set(&in_opts, "video_size", "320x240", 0);
    av_dict_set(&in_opts, "framerate", "30", 0);
    av_dict_set(&in_opts, "pattern", "gradient", 0);
    av_dict_set(&in_opts, "realtime", "0", 0);
    av_dict_set(&in_opts, "num_frames", "8", 0);

    const AVInputFormat *iformat = zff_find_input_format("zstr_videotestsrc");
    assert(iformat != NULL);
    int ret = avformat_open_input(&in_ctx, "dummy", iformat, &in_opts);
    assert(ret == 0);

    const AVOutputFormat *oformat = zff_find_output_format("zstr_x11sink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "display");
    assert(ret == 0);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width      = in_ctx->streams[0]->codecpar->width;
    st->codecpar->height     = in_ctx->streams[0]->codecpar->height;
    st->codecpar->format     = in_ctx->streams[0]->codecpar->format;
    st->time_base            = in_ctx->streams[0]->time_base;

    AVDictionary *out_opts = NULL;
    av_dict_set(&out_opts, "is_mock", "1", 0);
    ret = avformat_write_header(out_ctx, &out_opts);
    assert(ret == 0);

    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    int count = 0;
    while (av_read_frame(in_ctx, pkt) >= 0) {
        ret = av_write_frame(out_ctx, pkt);
        assert(ret == 0);
        count++;
        av_packet_unref(pkt);
    }
    assert(count == 8);

    av_packet_free(&pkt);
    av_write_trailer(out_ctx);
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);
    av_dict_free(&in_opts);
    av_dict_free(&out_opts);

    printf("[PASS] End-to-end loopback passed (8 frames rendered).\n");
}

static void test_x11sink_format_matrix_null(void) {
    printf("[TEST] zstr_x11sink null-mode write across pixel formats (no crash)...\n");

    int fmts[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV16, AV_PIX_FMT_YUYV422, AV_PIX_FMT_RGB24,
        AV_PIX_FMT_BGR24, AV_PIX_FMT_RGBA, AV_PIX_FMT_BGRA,
    };

    const AVOutputFormat *oformat = zff_find_output_format("zstr_x11sink");
    assert(oformat != NULL);

    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        AVFormatContext *out_ctx = NULL;
        int ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "display");
        assert(ret == 0);

        AVStream *st = avformat_new_stream(out_ctx, NULL);
        assert(st != NULL);
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->width      = 64;
        st->codecpar->height     = 48;
        st->codecpar->format     = fmts[i];
        st->time_base            = (AVRational){ 1, 30 };

        AVDictionary *opts = NULL;
        av_dict_set(&opts, "is_mock", "1", 0);
        ret = avformat_write_header(out_ctx, &opts);
        assert(ret == 0);

        int bufsize = av_image_get_buffer_size(fmts[i], 64, 48, 1);
        assert(bufsize > 0);
        AVPacket *pkt = av_packet_alloc();
        assert(pkt != NULL);
        ret = av_new_packet(pkt, bufsize);
        assert(ret == 0);
        memset(pkt->data, 0x80, pkt->size);
        pkt->stream_index = 0;

        ret = av_write_frame(out_ctx, pkt);
        assert(ret == 0);

        av_packet_free(&pkt);
        av_write_trailer(out_ctx);
        avformat_free_context(out_ctx);
        av_dict_free(&opts);
    }
    printf("[PASS] Format matrix null-mode passed.\n");
}

/* Real X server test: spawn Xvfb if needed, blit solid red, read back. */
static pid_t xvfb_pid = -1;

static const char *ensure_display(void) {
    const char *d = getenv("DISPLAY");
    if (d && d[0]) {
        Display *x = XOpenDisplay(d);
        if (x) { XCloseDisplay(x); return d; }
    }
    /* Try a private Xvfb instance */
    const char *priv = ":99";
    Display *x = XOpenDisplay(priv);
    if (x) { XCloseDisplay(x); setenv("DISPLAY", priv, 1); return priv; }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: Xvfb (silence output, never returns on success) */
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("Xvfb", "Xvfb", priv, "-screen", "0", "640x480x24", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return NULL;
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        x = XOpenDisplay(priv);
        if (x) {
            XCloseDisplay(x);
            xvfb_pid = pid;
            setenv("DISPLAY", priv, 1);
            return priv;
        }
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return NULL;
}

static void test_x11sink_real_display(void) {
    printf("[TEST] Testing zstr_x11sink against a real X server (Xvfb)...\n");

    const char *disp = ensure_display();
    if (!disp) {
        printf("[SKIP] No X server and Xvfb unavailable; skipping real-display test.\n");
        return;
    }
    printf("[INFO] Using DISPLAY=%s\n", disp);

    const AVOutputFormat *oformat = zff_find_output_format("zstr_x11sink");
    assert(oformat != NULL);

    AVFormatContext *out_ctx = NULL;
    int ret = avformat_alloc_output_context2(&out_ctx, oformat, NULL, "display");
    assert(ret == 0);

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    assert(st != NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width      = 160;
    st->codecpar->height     = 120;
    st->codecpar->format     = AV_PIX_FMT_RGB24;
    st->time_base            = (AVRational){ 1, 30 };

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "display", disp, 0);
    av_dict_set(&opts, "window_title", "zff-x11sink-test", 0);
    ret = avformat_write_header(out_ctx, &opts);
    assert(ret == 0);
    av_dict_free(&opts);

    /* Solid red frame */
    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);
    ret = av_new_packet(pkt, 160 * 120 * 3);
    assert(ret == 0);
    for (int i = 0; i < 160 * 120; i++) {
        pkt->data[i * 3 + 0] = 255;
        pkt->data[i * 3 + 1] = 0;
        pkt->data[i * 3 + 2] = 0;
    }
    pkt->stream_index = 0;
    ret = av_write_frame(out_ctx, pkt);
    assert(ret == 0);
    av_packet_free(&pkt);

    /* Read back the window content and check the center pixel is red-ish */
    Display *x = XOpenDisplay(disp);
    assert(x != NULL);
    XSync(x, False);
    Window root = DefaultRootWindow(x);
    Window rret, pret, *children = NULL;
    unsigned int nchildren = 0;
    XImage *shot = NULL;
    /* Find our window by title among root children */
    if (XQueryTree(x, root, &rret, &pret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren && !shot; i++) {
            char *name = NULL;
            if (XFetchName(x, children[i], &name) && name) {
                if (strcmp(name, "zff-x11sink-test") == 0) {
                    XSync(x, False);
                    shot = XGetImage(x, children[i], 0, 0, 160, 120, AllPlanes, ZPixmap);
                }
                XFree(name);
            }
        }
        if (children) XFree(children);
    }
    assert(shot != NULL);
    /* Center pixel: expect R high, G/B low (tolerant to depth conversion) */
    unsigned long px = XGetPixel(shot, 80, 60);
    int bpp = shot->bits_per_pixel;
    int r = 0, g = 0, bl = 0;
    if (bpp == 32 || bpp == 24) {
        /* Standard TrueColor LE layout: B,G,R,(X) */
        bl = (px >> 0) & 0xFF; g = (px >> 8) & 0xFF; r = (px >> 16) & 0xFF;
    } else {
        r = (px & 0xFF) ? 255 : 0; /* exotic depth: just check non-black */
    }
    printf("[INFO] Center pixel R=%d G=%d B=%d (bpp=%d)\n", r, g, bl, bpp);
    assert(r > 200 && g < 60 && bl < 60);
    XDestroyImage(shot);
    XCloseDisplay(x);

    ret = av_write_trailer(out_ctx);
    assert(ret == 0);
    avformat_free_context(out_ctx);

    printf("[PASS] Real-display XPutImage + readback passed.\n");
}

int main(void) {
    printf("====================================================\n");
    printf("           Running zstr X11Sink Tests               \n");
    printf("====================================================\n");

    zff_plugins_register_all();

    test_x11sink_mock_mode();
    test_loopback_videotestsrc_to_x11sink();
    test_x11sink_format_matrix_null();
    test_x11sink_real_display();

    if (xvfb_pid > 0) {
        kill(xvfb_pid, SIGTERM);
        waitpid(xvfb_pid, NULL, 0);
    }

    printf("====================================================\n");
    printf("       All zstr X11Sink Tests Passed!               \n");
    printf("====================================================\n");
    return 0;
}
