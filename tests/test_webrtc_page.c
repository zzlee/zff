/*=============================================================================
    test_webrtc_page.c — End-to-end page-demo test with a scripted browser

    Forks examples/webrtc_page/demo_webrtc_page, then plays the browser:
    WS JOIN, offer/answer/ICE, H264+Opus media, "chat" data channel echo,
    "KEYFRAME 0" request, and STATS observation. Verifies the demo's
    application wiring (not just the engine).
 =============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
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

#define DEMO_PORT 18610
#define DEMO_ROOM "pagetest"

static const char *demo_bin(void)
{
    const char *env = getenv("ZFF_DEMO_BIN");
    return (env && env[0]) ? env : "examples/webrtc_page/demo_webrtc_page";
}

typedef struct {
    zstr_webrtc_t *rtc;
    zstr_sig_client_t *sig;
    pthread_mutex_t lock;
    char *answer;
    bool answer_ready;
    char *pend_cand[16];
    char pend_mid[16][64];
    int nb_pend;
    bool remote_set;
    /* DC echo + stats */
    uint8_t echo[256];
    size_t echo_len;
    volatile int echo_got;
    uint64_t stat_frames;
    volatile int stat_got;
    pthread_cond_t cond;
} browser_t;

static browser_t g_b;

static void b_ice_cb(const char *cand, const char *mid, void *ud)
{
    browser_t *b = ud;
    if (b && b->sig && cand)
        zstr_sig_client_send_candidate(b->sig, mid ? mid : "", cand);
}

static void b_on_signal(zstr_sig_msg_t type, const char *payload,
                        const char *mid, void *ud)
{
    browser_t *b = ud;
    if (!b || !payload) return;
    pthread_mutex_lock(&b->lock);
    if (type == ZSTR_SIG_ANSWER) {
        free(b->answer);
        b->answer = strdup(payload);
        pthread_cond_signal(&b->cond);
    } else if (type == ZSTR_SIG_CANDIDATE) {
        if (b->remote_set) {
            zstr_webrtc_add_ice_candidate(b->rtc, payload, mid);
        } else if (b->nb_pend < 16) {
            snprintf(b->pend_mid[b->nb_pend], sizeof(b->pend_mid[0]),
                     "%s", mid ? mid : "");
            b->pend_cand[b->nb_pend] = strdup(payload);
            b->nb_pend++;
        }
    } else if (type == ZSTR_SIG_MSG) {
        unsigned long long f = 0;
        if (sscanf(payload, "STATS frames=%llu", &f) == 1 && f > 0) {
            b->stat_frames = f;
            b->stat_got = 1;
        }
    }
    pthread_mutex_unlock(&b->lock);
}

static void b_on_dc(const char *label, const uint8_t *data, size_t size,
                    bool is_binary, void *ud)
{
    (void)label;
    (void)is_binary;
    browser_t *b = ud;
    if (!b || !data || size == 0) return;
    size_t n = size < sizeof(b->echo) ? size : sizeof(b->echo);
    memcpy(b->echo, data, n);
    b->echo_len = n;
    b->echo_got = 1;
}

static void test_page_demo(void)
{
    printf("[TEST] Page demo end-to-end with scripted browser...\n");
    memset(&g_b, 0, sizeof(g_b));
    pthread_mutex_init(&g_b.lock, NULL);
    pthread_cond_init(&g_b.cond, NULL);

    /* Spawn the demo binary */
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%d", DEMO_PORT);
        execl(demo_bin(), demo_bin(), "--port", portstr, "--room", DEMO_ROOM,
              (char *)NULL);
        _exit(127);
    }
    usleep(800000); /* let demo bind + subscribe */

    char url[128];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d", DEMO_PORT);

    /* Browser engine: video+audio tracks, pre-offer "chat" data channel */
    g_b.rtc = zstr_webrtc_alloc("twcc=1");
    CHECK(g_b.rtc != NULL);
    CHECK(zstr_webrtc_add_video_track(g_b.rtc, ZSTR_WEBRTC_CODEC_H264, 126, 90000) == 0);
    CHECK(zstr_webrtc_add_audio_track(g_b.rtc, ZSTR_WEBRTC_CODEC_OPUS, 111, 48000) == 1);
    CHECK(zstr_webrtc_create_data_channel(g_b.rtc, "chat") == 0);
    zstr_webrtc_set_ice_cb(g_b.rtc, b_ice_cb, &g_b);
    zstr_webrtc_set_dc_message_cb(g_b.rtc, b_on_dc, &g_b);

    g_b.sig = zstr_sig_client_connect(url, DEMO_ROOM, b_on_signal, &g_b, 10000);
    CHECK(g_b.sig != NULL);

    const char *offer = zstr_webrtc_create_offer(g_b.rtc);
    CHECK(offer != NULL);
    char *offer_copy = strdup(offer);
    CHECK(offer_copy != NULL);
    CHECK(zstr_sig_client_send_offer(g_b.sig, offer_copy) == 0);
    free(offer_copy);

    /* Wait for the demo's answer */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 20;
    pthread_mutex_lock(&g_b.lock);
    while (!g_b.answer)
        if (pthread_cond_timedwait(&g_b.cond, &g_b.lock, &ts) == ETIMEDOUT) break;
    char *answer = g_b.answer;
    g_b.answer = NULL;
    pthread_mutex_unlock(&g_b.lock);
    CHECK(answer != NULL);
    CHECK(zstr_webrtc_set_remote_description(g_b.rtc, answer, "answer") == 0);
    free(answer);

    pthread_mutex_lock(&g_b.lock);
    g_b.remote_set = true;
    for (int i = 0; i < g_b.nb_pend; i++) {
        zstr_webrtc_add_ice_candidate(g_b.rtc, g_b.pend_cand[i], g_b.pend_mid[i]);
        free(g_b.pend_cand[i]);
    }
    g_b.nb_pend = 0;
    pthread_mutex_unlock(&g_b.lock);

    CHECK(zstr_webrtc_wait_connected(g_b.rtc, 20000) == 0);
    printf("[INFO] Browser side connected.\n");

    /* Publish a little media */
    uint8_t h264[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x99, 0x88, 0x77 };
    for (int i = 0; i < 4; i++) {
        AVPacket *pkt = av_packet_alloc();
        av_new_packet(pkt, sizeof(h264));
        memcpy(pkt->data, h264, sizeof(h264));
        pkt->pts = i * 3000;
        pkt->time_base = (AVRational){ 1, 90000 };
        CHECK(zstr_webrtc_send_media(g_b.rtc, 0, pkt) == 0);
        av_packet_free(&pkt);
    }

    /* Data channel echo check */
    const char *hello = "page-hello";
    int sent = 0;
    for (int i = 0; i < 100 && !sent; i++) {
        if (zstr_webrtc_send_data(g_b.rtc, "chat",
                                  (const uint8_t *)hello, strlen(hello)) == 0)
            sent = 1;
        else
            usleep(50000);
    }
    CHECK(sent);
    for (int i = 0; i < 100 && !g_b.echo_got; i++) usleep(50000);
    CHECK(g_b.echo_got);
    CHECK(g_b.echo_len == strlen(hello));
    CHECK(memcmp(g_b.echo, hello, strlen(hello)) == 0);
    printf("[INFO] Data channel echo verified.\n");

    /* Ask for a keyframe (demo logs it; WWAN delivery is best-effort) */
    CHECK(zstr_sig_client_send_msg(g_b.sig, "KEYFRAME 0") == 0);

    /* Observe at least one STATS with frames > 0 */
    for (int i = 0; i < 100 && !g_b.stat_got; i++) usleep(100000);
    CHECK(g_b.stat_got);
    printf("[INFO] Demo reported %llu frames received.\n",
           (unsigned long long)g_b.stat_frames);

    zstr_sig_client_free(&g_b.sig);
    zstr_webrtc_free(&g_b.rtc);

    kill(child, SIGTERM);
    int status = 0;
    pid_t w = waitpid(child, &status, 0);
    CHECK(w == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    pthread_mutex_destroy(&g_b.lock);
    pthread_cond_destroy(&g_b.cond);
    printf("[PASS] Page demo end-to-end passed.\n");
}

int main(void)
{
    printf("====================================================\n");
    printf("        Running zstr Page Demo Tests                 \n");
    printf("====================================================\n");

    zff_plugins_register_all();
    test_page_demo();

    printf("====================================================\n");
    printf("     All Page Demo Tests Passed!                    \n");
    printf("====================================================\n");
    return 0;
}
