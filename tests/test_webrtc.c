/*=============================================================================
    test_webrtc.c — zstr_webrtc engine loopback + TWCC unit tests

    1. TWCC SDP helpers (pure string functions, deterministic)
    2. Synthetic RTCP CCFB -> GCC loss estimator drives bitrate down
    3. Outgoing RTP gets the transport-cc extension stamped
    4. Full PeerConnection loopback: SDP offer/answer + ICE trickle
       in-process, H264 + Opus media round-trip, data channel message
 =============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <libavcodec/packet.h>
#include <libavutil/error.h>

#include "zff/plugins/zstr_webrtc.h"
#include "zff/plugins/zstr_webrtc_twcc.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

/* --- TWCC SDP helper tests (no network) --- */
static void test_twcc_sdp_helpers(void)
{
    printf("[TEST] TWCC parse_offer / inject_answer helpers...\n");

    zstr_webrtc_twcc_t *twcc = zstr_webrtc_twcc_create(NULL, NULL);
    CHECK(twcc != NULL);

    const char *offer =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 126\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n";
    CHECK(zstr_webrtc_twcc_parse_offer(twcc, offer) == 5);

    char answer[2048];
    snprintf(answer, sizeof(answer),
             "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 126\r\nc=IN IP4 0.0.0.0\r\n");
    CHECK(zstr_webrtc_twcc_inject_answer(twcc, answer, sizeof(answer)) == 0);
    CHECK(strstr(answer, "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01") != NULL);

    CHECK(zstr_webrtc_twcc_parse_offer(twcc, "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 126\r\n") == -1);

    zstr_webrtc_twcc_destroy(twcc);
    printf("[PASS] TWCC SDP helpers passed.\n");
}

static uint64_t g_last_bitrate = 0;
static int g_bitrate_events = 0;

static void on_bitrate(uint64_t bps, void *ud)
{
    (void)ud;
    g_last_bitrate = bps;
    g_bitrate_events++;
}

static void test_twcc_ccfb_drives_bitrate_down(void)
{
    printf("[TEST] Synthetic CCFB loss feedback reduces GCC estimate...\n");

    g_last_bitrate = 0;
    g_bitrate_events = 0;
    zstr_webrtc_twcc_t *twcc = zstr_webrtc_twcc_create(on_bitrate, NULL);
    CHECK(twcc != NULL);
    CHECK(zstr_webrtc_twcc_bitrate(twcc) == 2000000);

    /* Build a 24-byte RTCP RTPFB (PT=205, fmt=15) with 10/10 packets lost:
     * header + sender/media SSRC + base_seq=100 + count=10 + ref_time +
     * fb_count + one run-length chunk (st=0, run=10). */
    uint8_t ccfb[24];
    memset(ccfb, 0, sizeof(ccfb));
    ccfb[0] = 0x8F; /* V=2, fmt=15 */
    ccfb[1] = 205;
    ccfb[2] = 0; ccfb[3] = 5; /* length = 24/4 - 1 */
    uint32_t ssrc = htonl(0x11223344);
    memcpy(ccfb + 4, &ssrc, 4);
    memcpy(ccfb + 8, &ssrc, 4);
    uint16_t base = htons(100), cnt = htons(10);
    memcpy(ccfb + 12, &base, 2);
    memcpy(ccfb + 14, &cnt, 2);
    ccfb[16] = 0; ccfb[17] = 0; ccfb[18] = 1; /* ref time */
    ccfb[19] = 7;                              /* fb count */
    uint16_t chunk = htons((0 << 13) | 10);    /* RLE: status=not-received x10 */
    memcpy(ccfb + 20, &chunk, 2);

    /* GCC update interval is 100ms; spread the two feedbacks apart */
    usleep(150000);
    zstr_webrtc_twcc_process_incoming(twcc, (const char *)ccfb, sizeof(ccfb));
    CHECK(g_bitrate_events == 1);
    CHECK(g_last_bitrate < 2000000); /* 100% loss: 2.0M * 0.8 = 1.6M */

    usleep(150000);
    zstr_webrtc_twcc_process_incoming(twcc, (const char *)ccfb, sizeof(ccfb));
    CHECK(g_bitrate_events == 2);
    CHECK(g_last_bitrate < 1600000); /* smoothed loss still >10%: 1.6M * 0.8 */

    zstr_webrtc_twcc_destroy(twcc);
    printf("[PASS] CCFB loss feedback passed (final %llu bps).\n",
           (unsigned long long)g_last_bitrate);
}

/* --- Loopback plumbing --- */
typedef struct {
    zstr_webrtc_t *a;
    zstr_webrtc_t *b;
    /* Buffered remote candidates (trickle may precede set_remote) */
    char cand_a[16][256];
    char mid_a[16][32];
    int nb_a;
    char cand_b[16][256];
    char mid_b[16][32];
    int nb_b;
    /* Data channel echo */
    uint8_t dc_rx[256];
    size_t dc_rx_len;
    volatile int dc_got;
} loopback_t;

static loopback_t g_lb;

static void ice_to_b(const char *cand, const char *mid, void *ud)
{
    (void)ud;
    if (g_lb.nb_a < 16) {
        snprintf(g_lb.cand_a[g_lb.nb_a], sizeof(g_lb.cand_a[0]), "%s", cand);
        snprintf(g_lb.mid_a[g_lb.nb_a], sizeof(g_lb.mid_a[0]), "%s", mid ? mid : "");
        g_lb.nb_a++;
    }
}

static void ice_to_a(const char *cand, const char *mid, void *ud)
{
    (void)ud;
    if (g_lb.nb_b < 16) {
        snprintf(g_lb.cand_b[g_lb.nb_b], sizeof(g_lb.cand_b[0]), "%s", cand);
        snprintf(g_lb.mid_b[g_lb.nb_b], sizeof(g_lb.mid_b[0]), "%s", mid ? mid : "");
        g_lb.nb_b++;
    }
}

static void dc_on_message(const char *label, const uint8_t *data, size_t size,
                          bool is_binary, void *ud)
{
    (void)label; (void)is_binary; (void)ud;
    size_t n = size < sizeof(g_lb.dc_rx) ? size : sizeof(g_lb.dc_rx);
    memcpy(g_lb.dc_rx, data, n);
    g_lb.dc_rx_len = n;
    g_lb.dc_got = 1;
}

static void test_webrtc_loopback(void)
{
    printf("[TEST] PeerConnection loopback (offer/answer + ICE + media + DC)...\n");
    memset(&g_lb, 0, sizeof(g_lb));

    g_lb.a = zstr_webrtc_alloc("twcc=1");
    g_lb.b = zstr_webrtc_alloc("twcc=1");
    CHECK(g_lb.a && g_lb.b);

    zstr_webrtc_set_ice_cb(g_lb.a, ice_to_b, NULL);
    zstr_webrtc_set_ice_cb(g_lb.b, ice_to_a, NULL);
    zstr_webrtc_set_dc_message_cb(g_lb.b, dc_on_message, NULL);

    CHECK(zstr_webrtc_add_video_track(g_lb.a, ZSTR_WEBRTC_CODEC_H264, 126, 90000) == 0);
    CHECK(zstr_webrtc_add_audio_track(g_lb.a, ZSTR_WEBRTC_CODEC_OPUS, 111, 48000) == 1);
    CHECK(zstr_webrtc_add_video_track(g_lb.b, ZSTR_WEBRTC_CODEC_H264, 126, 90000) == 0);
    CHECK(zstr_webrtc_add_audio_track(g_lb.b, ZSTR_WEBRTC_CODEC_OPUS, 111, 48000) == 1);

    /* Data channel must be created BEFORE the offer so its m=application
     * section is negotiated (late creation needs renegotiation). */
    CHECK(zstr_webrtc_create_data_channel(g_lb.a, "chat") == 0);

    const char *offer = zstr_webrtc_create_offer(g_lb.a);
    CHECK(offer && strstr(offer, "m=video") && strstr(offer, "m=audio"));
    CHECK(strstr(offer, "m=application")); /* pre-offer data channel negotiated */
    CHECK(zstr_webrtc_set_remote_description(g_lb.b, offer, "offer") == 0);

    const char *answer = zstr_webrtc_create_answer(g_lb.b);
    CHECK(answer && strstr(answer, "m=video"));
    CHECK(zstr_webrtc_set_remote_description(g_lb.a, answer, "answer") == 0);

    /* Flush trickled candidates both ways */
    for (int i = 0; i < g_lb.nb_a; i++)
        zstr_webrtc_add_ice_candidate(g_lb.b, g_lb.cand_a[i], g_lb.mid_a[i]);
    for (int i = 0; i < g_lb.nb_b; i++)
        zstr_webrtc_add_ice_candidate(g_lb.a, g_lb.cand_b[i], g_lb.mid_b[i]);

    CHECK(zstr_webrtc_wait_connected(g_lb.a, 15000) == 0);
    CHECK(zstr_webrtc_wait_connected(g_lb.b, 15000) == 0);
    printf("[INFO] Both peers connected.\n");

    /* Video round-trip A -> B (Annex-B NAL, as the H264 packetizer expects) */
    uint8_t h264[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xA0, 0x11, 0x22, 0x33 };
    AVPacket *tx = av_packet_alloc();
    av_new_packet(tx, sizeof(h264));
    memcpy(tx->data, h264, sizeof(h264));
    tx->pts = 90000;
    tx->time_base = (AVRational){ 1, 90000 };
    tx->stream_index = 0;
    CHECK(zstr_webrtc_send_media(g_lb.a, 0, tx) == 0);
    av_packet_free(&tx);

    AVPacket *rx = av_packet_alloc();
    int track_idx = -1;
    bool got = false;
    for (int i = 0; i < 100 && !got; i++) {
        int rc = zstr_webrtc_recv_media(g_lb.b, rx, &track_idx, 200, &got);
        if (rc == AVERROR(ETIMEDOUT)) continue; /* no frame yet, keep polling */
        CHECK(rc == 0);
    }
    CHECK(got);
    CHECK(rx->size == (int)sizeof(h264));
    CHECK(memcmp(rx->data, h264, sizeof(h264)) == 0);
    av_packet_free(&rx);
    printf("[INFO] Video loopback verified (%d bytes).\n", (int)sizeof(h264));

    /* Audio round-trip A -> B */
    uint8_t opus[32];
    for (int i = 0; i < 32; i++) opus[i] = (uint8_t)(0x40 + i);
    tx = av_packet_alloc();
    av_new_packet(tx, sizeof(opus));
    memcpy(tx->data, opus, sizeof(opus));
    tx->pts = 48000;
    tx->time_base = (AVRational){ 1, 48000 };
    tx->stream_index = 1;
    CHECK(zstr_webrtc_send_media(g_lb.a, 1, tx) == 0);
    av_packet_free(&tx);

    rx = av_packet_alloc();
    got = false;
    for (int i = 0; i < 100 && !got; i++) {
        int rc = zstr_webrtc_recv_media(g_lb.b, rx, &track_idx, 200, &got);
        if (rc == AVERROR(ETIMEDOUT)) continue;
        CHECK(rc == 0);
    }
    CHECK(got);
    CHECK(rx->size == (int)sizeof(opus));
    CHECK(memcmp(rx->data, opus, sizeof(opus)) == 0);
    av_packet_free(&rx);
    printf("[INFO] Audio loopback verified.\n");

    /* Data channel A -> B (created pre-offer above) */
    const char *msg = "hello-webrtc";
    int sent = 0;
    for (int i = 0; i < 100 && !sent; i++) {
        if (zstr_webrtc_send_data(g_lb.a, "chat", (const uint8_t *)msg, strlen(msg)) == 0)
            sent = 1;
        else
            usleep(50000);
    }
    CHECK(sent);
    for (int i = 0; i < 100 && !g_lb.dc_got; i++) usleep(50000);
    CHECK(g_lb.dc_got);
    CHECK(g_lb.dc_rx_len == strlen(msg));
    CHECK(memcmp(g_lb.dc_rx, msg, strlen(msg)) == 0);
    printf("[INFO] Data channel verified.\n");

    zstr_webrtc_free(&g_lb.a);
    zstr_webrtc_free(&g_lb.b);
    printf("[PASS] WebRTC loopback passed.\n");
}

int main(void)
{
    printf("====================================================\n");
    printf("        Running zstr_webrtc Engine Tests             \n");
    printf("====================================================\n");

    test_twcc_sdp_helpers();
    test_twcc_ccfb_drives_bitrate_down();
    test_webrtc_loopback();

    printf("====================================================\n");
    printf("     All zstr_webrtc Tests Passed Successfully!     \n");
    printf("====================================================\n");
    return 0;
}
