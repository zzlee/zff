/*=============================================================================
    test_rtsp_server.c — Integration test for zstr_rtspserver

    Drives a full RTSP handshake over a raw TCP socket against the muxer:
      OPTIONS -> DESCRIBE (SDP check) -> SETUP video (TCP) ->
      SETUP audio (TCP) -> PLAY -> av_write_frame video+audio ->
      verify interleaved RTP ($) -> TEARDOWN

    Ported scenario from zstreamer demo_rtsp_mod / test expectations.
 =============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "zff/plugins/zstr_rtsp_server.h"
#include "zff/zff_core.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

#define TEST_PORT 18554
#define RECV_CAP 65536

static int g_sock = -1;

static void sock_connect(void)
{
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(g_sock >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
}

static void sock_send(const char *req)
{
    size_t len = strlen(req);
    CHECK(send(g_sock, req, len, 0) == (ssize_t)len);
}

/* Read until full RTSP header (\r\n\r\n) plus optional body per Content-Length */
static int sock_recv_reply(char *out, int cap)
{
    int total = 0;
    int body_len = -1;
    for (;;) {
        struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
        CHECK(poll(&pfd, 1, 3000) > 0);
        ssize_t n = recv(g_sock, out + total, cap - total - 1, 0);
        CHECK(n > 0);
        total += (int)n;
        out[total] = '\0';
        char *hdr_end = strstr(out, "\r\n\r\n");
        if (!hdr_end) continue;
        if (body_len < 0) {
            body_len = 0;
            char *cl = strstr(out, "Content-Length:");
            if (cl && cl < hdr_end) body_len = atoi(cl + 15);
        }
        if (total >= (hdr_end - out) + 4 + body_len) return total;
    }
}

static void expect_contains(const char *reply, const char *needle, const char *what)
{
    if (!strstr(reply, needle)) {
        fprintf(stderr, "FAIL: %s not found in reply:\n%s\n", what, reply);
        abort();
    }
}

static char g_session[64] = "";

static void extract_session(const char *reply)
{
    const char *p = strstr(reply, "Session:");
    assert(p != NULL);
    p += 8;
    while (*p == ' ') p++;
    int i = 0;
    while (*p && *p != '\r' && *p != ';' && i + 1 < (int)sizeof(g_session))
        g_session[i++] = *p++;
    g_session[i] = '\0';
    assert(i > 0);
}

/* Minimal avcC extradata: 1 SPS (13B) + 1 PPS (4B), content opaque for SDP */
static const uint8_t FAKE_AVCC[] = {
    0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x0d,
    0x67, 0x42, 0x00, 0x1f, 0x96, 0x54, 0x05, 0x01,
    0xed, 0x80, 0x80, 0x80, 0xa0, 0x01, 0x00, 0x04,
    0x68, 0xce, 0x3c, 0x80
};

/* Annex-B video AU: SPS + PPS + IDR (small, single-packet each) */
static const uint8_t FAKE_AU[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0
};

static const uint8_t FAKE_AAC[] = {
    0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0x00, 0x00
};

static void test_rtsp_handshake_and_rtp(void)
{
    printf("[TEST] RTSP handshake + RTP fan-out on port %d...\n", TEST_PORT);

    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_rtspserver");
    assert(out_fmt != NULL);

    AVFormatContext *oc = NULL;
    char url[128];
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%d/live", TEST_PORT);
    CHECK(avformat_alloc_output_context2(&oc, out_fmt, "zstr_rtspserver", url) >= 0);

    AVStream *vst = avformat_new_stream(oc, NULL);
    assert(vst != NULL);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id = AV_CODEC_ID_H264;
    vst->codecpar->width = 640;
    vst->codecpar->height = 480;
    vst->codecpar->extradata = av_malloc(sizeof(FAKE_AVCC));
    memcpy(vst->codecpar->extradata, FAKE_AVCC, sizeof(FAKE_AVCC));
    vst->codecpar->extradata_size = sizeof(FAKE_AVCC);
    vst->time_base = (AVRational){ 1, 90000 };

    AVStream *ast = avformat_new_stream(oc, NULL);
    assert(ast != NULL);
    ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    ast->codecpar->codec_id = AV_CODEC_ID_AAC;
    ast->codecpar->sample_rate = 44100;
    ast->codecpar->ch_layout.nb_channels = 2;
    av_channel_layout_default(&ast->codecpar->ch_layout, 2);
    ast->time_base = (AVRational){ 1, 44100 };

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "port", "18554", 0);
    CHECK(avformat_write_header(oc, &opts) >= 0);
    av_dict_free(&opts);

    char reply[RECV_CAP];
    char req[1024];

    sock_connect();

    /* OPTIONS */
    snprintf(req, sizeof(req),
        "OPTIONS rtsp://127.0.0.1:%d/live RTSP/1.0\r\nCSeq: 1\r\n\r\n", TEST_PORT);
    sock_send(req);
    sock_recv_reply(reply, sizeof(reply));
    expect_contains(reply, "200 OK", "OPTIONS status");

    /* DESCRIBE */
    snprintf(req, sizeof(req),
        "DESCRIBE rtsp://127.0.0.1:%d/live RTSP/1.0\r\n"
        "CSeq: 2\r\nAccept: application/sdp\r\n\r\n", TEST_PORT);
    sock_send(req);
    sock_recv_reply(reply, sizeof(reply));
    expect_contains(reply, "200 OK", "DESCRIBE status");
    expect_contains(reply, "m=video", "SDP video");
    expect_contains(reply, "m=audio", "SDP audio");
    expect_contains(reply, "sprop-parameter-sets=", "SDP sprop");
    expect_contains(reply, "MPEG4-GENERIC", "SDP AAC");

    /* SETUP video over TCP */
    snprintf(req, sizeof(req),
        "SETUP rtsp://127.0.0.1:%d/live/trackID=0 RTSP/1.0\r\n"
        "CSeq: 3\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n", TEST_PORT);
    sock_send(req);
    sock_recv_reply(reply, sizeof(reply));
    expect_contains(reply, "200 OK", "SETUP video status");
    expect_contains(reply, "interleaved=0-1", "SETUP video transport");
    extract_session(reply);

    /* SETUP audio over TCP */
    snprintf(req, sizeof(req),
        "SETUP rtsp://127.0.0.1:%d/live/trackID=1 RTSP/1.0\r\n"
        "CSeq: 4\r\nSession: %s\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n", TEST_PORT, g_session);
    sock_send(req);
    sock_recv_reply(reply, sizeof(reply));
    expect_contains(reply, "200 OK", "SETUP audio status");
    expect_contains(reply, "interleaved=2-3", "SETUP audio transport");

    /* PLAY */
    snprintf(req, sizeof(req),
        "PLAY rtsp://127.0.0.1:%d/live RTSP/1.0\r\n"
        "CSeq: 5\r\nSession: %s\r\nRange: npt=0.000-\r\n\r\n", TEST_PORT, g_session);
    sock_send(req);
    sock_recv_reply(reply, sizeof(reply));
    expect_contains(reply, "200 OK", "PLAY status");
    expect_contains(reply, "RTP-Info:", "PLAY RTP-Info");

    /* Push one video AU + one audio frame */
    AVPacket *vpkt = av_packet_alloc();
    av_new_packet(vpkt, sizeof(FAKE_AU));
    memcpy(vpkt->data, FAKE_AU, sizeof(FAKE_AU));
    vpkt->stream_index = 0;
    vpkt->pts = 0;
    CHECK(av_write_frame(oc, vpkt) == 0);
    av_packet_free(&vpkt);

    AVPacket *apkt = av_packet_alloc();
    av_new_packet(apkt, sizeof(FAKE_AAC));
    memcpy(apkt->data, FAKE_AAC, sizeof(FAKE_AAC));
    apkt->stream_index = 1;
    apkt->pts = 0;
    CHECK(av_write_frame(oc, apkt) == 0);
    av_packet_free(&apkt);

    /* Read interleaved RTP: expect '$', video channel 0, RTP v2 */
    uint8_t frame[RECV_CAP];
    int got_video = 0, got_audio = 0;
    for (int i = 0; i < 8 && !(got_video && got_audio); i++) {
        struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
        CHECK(poll(&pfd, 1, 3000) > 0);
        uint8_t hdr[4];
        int off = 0;
        while (off < 4) {
            ssize_t n = recv(g_sock, hdr + off, 4 - off, 0);
            CHECK(n > 0);
            off += (int)n;
        }
        assert(hdr[0] == '$');
        int ch = hdr[1];
        int len = (hdr[2] << 8) | hdr[3];
        assert(len > 12 && len < RECV_CAP);
        off = 0;
        while (off < len) {
            ssize_t n = recv(g_sock, frame + off, len - off, 0);
            CHECK(n > 0);
            off += (int)n;
        }
        assert((frame[0] & 0xC0) == 0x80); /* RTP v2 */
        if (ch == 0) {
            assert((frame[1] & 0x7F) == 96); /* H264 PT */
            got_video = 1;
        } else if (ch == 2) {
            assert((frame[1] & 0x7F) == 97); /* AAC PT */
            assert(frame[12] == 0 && frame[13] == 16); /* AU-headers-length */
            got_audio = 1;
        }
        (void)frame;
    }
    assert(got_video && got_audio);

    /* TEARDOWN */
    snprintf(req, sizeof(req),
        "TEARDOWN rtsp://127.0.0.1:%d/live RTSP/1.0\r\n"
        "CSeq: 6\r\nSession: %s\r\n\r\n", TEST_PORT, g_session);
    sock_send(req);
    sock_recv_reply(reply, sizeof(reply));
    expect_contains(reply, "200 OK", "TEARDOWN status");

    close(g_sock);
    g_sock = -1;
    av_write_trailer(oc);
    avformat_free_context(oc);

    printf("[PASS] RTSP handshake + RTP fan-out passed.\n");
}

static void test_unknown_mount_404(void)
{
    printf("[TEST] Unknown mount returns 404...\n");
    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_rtspserver");
    AVFormatContext *oc = NULL;
    char url[128];
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%d/live", TEST_PORT + 1);
    CHECK(avformat_alloc_output_context2(&oc, out_fmt, "zstr_rtspserver", url) >= 0);
    AVStream *vst = avformat_new_stream(oc, NULL);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id = AV_CODEC_ID_H264;
    vst->codecpar->width = 640;
    vst->codecpar->height = 480;
    vst->time_base = (AVRational){ 1, 90000 };
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "port", "18555", 0);
    CHECK(avformat_write_header(oc, &opts) >= 0);
    av_dict_free(&opts);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 1);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    const char *req = "DESCRIBE rtsp://127.0.0.1:18555/nosuch RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    CHECK(send(fd, req, strlen(req), 0) == (ssize_t)strlen(req));
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    CHECK(poll(&pfd, 1, 3000) > 0);
    char reply[4096];
    ssize_t n = recv(fd, reply, sizeof(reply) - 1, 0);
    CHECK(n > 0);
    reply[n] = '\0';
    expect_contains(reply, "404", "unknown mount 404");
    close(fd);

    av_write_trailer(oc);
    avformat_free_context(oc);
    printf("[PASS] Unknown mount 404 passed.\n");
}

int main(void)
{
    zff_plugins_register_all();
    test_rtsp_handshake_and_rtp();
    test_unknown_mount_404();
    printf("All RTSP server tests passed.\n");
    return 0;
}
