/*=============================================================================

    test_webrtc_signaling.c — WebSocket signaling switch + one-call handshake

    In-process zstr_sig_server, then offerer (main) + answerer (thread)

    connect via zstr_webrtc_connect_signaling and exchange H264 + Opus.

 =============================================================================*/

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <assert.h>

#include <unistd.h>

#include <errno.h>

#include <pthread.h>

#include <libavcodec/packet.h>

#include <libavutil/error.h>

#include "zff/plugins/zstr_webrtc.h"

#include "zff/plugins/zstr_signaling.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

static char g_url[128];

static const char *g_room = "sigtest";

typedef struct {

    int got_video;

    int got_audio;

    int rc;

} answerer_result_t;

static void *answerer_thread(void *arg)

{

    answerer_result_t *res = arg;

    zstr_webrtc_t *b = zstr_webrtc_alloc("twcc=1");

    if (!b) {

        res->rc = -1;

        return NULL;

    }

    if (zstr_webrtc_add_video_track(b, ZSTR_WEBRTC_CODEC_H264, 126, 90000) < 0 ||

        zstr_webrtc_add_audio_track(b, ZSTR_WEBRTC_CODEC_OPUS, 111, 48000) < 0) {

        res->rc = -2;

        zstr_webrtc_free(&b);

        return NULL;

    }

    if (zstr_webrtc_connect_signaling(b, g_url, g_room, false, 25000) < 0) {

        res->rc = -3;

        zstr_webrtc_free(&b);

        return NULL;

    }

    AVPacket *rx = av_packet_alloc();

    int track_idx = -1;

    bool got = false;

    for (int i = 0; i < 100 && (!res->got_video || !res->got_audio); i++) {

        int rc = zstr_webrtc_recv_media(b, rx, &track_idx, 200, &got);

        if (rc == AVERROR(ETIMEDOUT)) continue;

        if (rc < 0 || !got) break;

        if (rx->size == 8 && rx->data[4] == 0x65) res->got_video++;

        else if (rx->size == 16 && rx->data[0] == 0xBB) res->got_audio++;

        av_packet_unref(rx);

    }

    av_packet_free(&rx);

    zstr_webrtc_free(&b);

    res->rc = (res->got_video >= 1 && res->got_audio >= 1) ? 0 : -4;

    return NULL;

}

static void test_signaling_handshake_and_media(void)

{

    printf("[TEST] WebSocket signaling handshake + media loopback...\n");

    zstr_sig_server_t *srv = zstr_sig_server_start(0);

    CHECK(srv != NULL);

    int port = zstr_sig_server_port(srv);

    CHECK(port > 0);

    snprintf(g_url, sizeof(g_url), "ws://127.0.0.1:%d", port);

    printf("[INFO] Signaling server on %s\n", g_url);

    answerer_result_t res;

    memset(&res, 0, sizeof(res));

    pthread_t th;

    CHECK(pthread_create(&th, NULL, answerer_thread, &res) == 0);

    usleep(200000); /* let the answerer connect + subscribe first */

    zstr_webrtc_t *a = zstr_webrtc_alloc("twcc=1");

    CHECK(a != NULL);

    CHECK(zstr_webrtc_add_video_track(a, ZSTR_WEBRTC_CODEC_H264, 126, 90000) == 0);

    CHECK(zstr_webrtc_add_audio_track(a, ZSTR_WEBRTC_CODEC_OPUS, 111, 48000) == 1);

    CHECK(zstr_webrtc_connect_signaling(a, g_url, g_room, true, 25000) == 0);

    printf("[INFO] Offerer connected via signaling.\n");

    uint8_t h264[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x77, 0x66 };

    AVPacket *tx = av_packet_alloc();

    av_new_packet(tx, sizeof(h264));

    memcpy(tx->data, h264, sizeof(h264));

    tx->pts = 90000;

    tx->time_base = (AVRational){ 1, 90000 };

    CHECK(zstr_webrtc_send_media(a, 0, tx) == 0);

    av_packet_free(&tx);

    uint8_t opus[16];

    memset(opus, 0xBB, sizeof(opus));

    tx = av_packet_alloc();

    av_new_packet(tx, sizeof(opus));

    memcpy(tx->data, opus, sizeof(opus));

    tx->pts = 48000;

    tx->time_base = (AVRational){ 1, 48000 };

    CHECK(zstr_webrtc_send_media(a, 1, tx) == 0);

    av_packet_free(&tx);

    pthread_join(th, NULL);

    CHECK(res.rc == 0);

    CHECK(res.got_video >= 1);

    CHECK(res.got_audio >= 1);

    zstr_webrtc_free(&a);

    zstr_sig_server_free(&srv);

    printf("[PASS] Signaling handshake + media loopback passed.\n");

}

typedef struct {
    char text[256];
    volatile int got;
} msg_result_t;

static void msg_on_message(zstr_sig_msg_t type, const char *payload,
                           const char *mid, void *ud)
{
    (void)mid;
    msg_result_t *r = ud;
    if (type == ZSTR_SIG_MSG && payload) {
        snprintf(r->text, sizeof(r->text), "%s", payload);
        r->got = 1;
    }
}

static void test_signaling_msg_routing(void)
{
    printf("[TEST] MSG routing between two clients...\n");

    zstr_sig_server_t *srv = zstr_sig_server_start(0);
    CHECK(srv != NULL);
    int port = zstr_sig_server_port(srv);
    CHECK(port > 0);
    char url[128];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d", port);

    msg_result_t ra;
    memset(&ra, 0, sizeof(ra));
    msg_result_t rb;
    memset(&rb, 0, sizeof(rb));

    zstr_sig_client_t *ca =
        zstr_sig_client_connect(url, "msgroom", msg_on_message, &ra, 5000);
    CHECK(ca != NULL);
    zstr_sig_client_t *cb =
        zstr_sig_client_connect(url, "msgroom", msg_on_message, &rb, 5000);
    CHECK(cb != NULL);
    /* Different room must NOT receive it */
    msg_result_t rc;
    memset(&rc, 0, sizeof(rc));
    zstr_sig_client_t *cc =
        zstr_sig_client_connect(url, "otherroom", msg_on_message, &rc, 5000);
    CHECK(cc != NULL);

    usleep(200000); /* let JOINs land */
    CHECK(zstr_sig_client_send_msg(ca, "hello-b") == 0);

    for (int i = 0; i < 50 && !rb.got; i++) usleep(100000);
    CHECK(rb.got == 1);
    CHECK(strcmp(rb.text, "hello-b") == 0);
    CHECK(rc.got == 0);

    zstr_sig_client_free(&ca);
    zstr_sig_client_free(&cb);
    zstr_sig_client_free(&cc);
    zstr_sig_server_free(&srv);

    printf("[PASS] MSG routing passed.\n");

}

int main(void)

{

    printf("====================================================\n");

    printf("      Running zstr WebSocket Signaling Tests        \n");

    printf("====================================================\n");

    test_signaling_handshake_and_media();

    test_signaling_msg_routing();

    printf("====================================================\n");

    printf("   All Signaling Tests Passed!                      \n");

    printf("====================================================\n");

    return 0;

}
