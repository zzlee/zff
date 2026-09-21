/*=============================================================================
    demo.c — Browser publishing demo for zstr_webrtc

    A browser (examples/webrtc_page/index.html) publishes camera/mic over
    WebRTC; this program receives, prints live stats, echoes the "chat"
    data channel, and honors "KEYFRAME <idx>" requests from the page.

    Run: ./demo_webrtc_page [--port 8610] [--room demo]
    Then open index.html in Chrome (localhost = secure context for camera).
 =============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include <libavcodec/packet.h>
#include <libavutil/error.h>

#include "zff/plugins/zstr_webrtc.h"
#include "zff/plugins/zstr_signaling.h"

static volatile int g_running = 1;

static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

typedef struct {
    zstr_webrtc_t *rtc;
    zstr_sig_client_t *sig;
    pthread_mutex_t lock;
    /* Buffered remote ICE until set_remote completes */
    char pend_mid[32][64];
    char *pend_cand[32];
    int nb_pend;
    bool remote_set;
    /* Stats */
    uint64_t rx_frames[4];
    uint64_t rx_bytes[4];
    uint64_t stat_tick;
} demo_t;

static demo_t g_demo;

static void demo_ice_cb(const char *cand, const char *mid, void *ud)
{
    demo_t *d = ud;
    if (d && d->sig && cand)
        zstr_sig_client_send_candidate(d->sig, mid ? mid : "", cand);
}

static void demo_on_signal(zstr_sig_msg_t type, const char *payload,
                           const char *mid, void *ud)
{
    demo_t *d = ud;
    if (!d || !payload) return;

    if (type == ZSTR_SIG_OFFER) {
        printf("[demo] Offer received (%zu bytes), answering...\n", strlen(payload));
        if (zstr_webrtc_set_remote_description(d->rtc, payload, "offer") < 0) {
            fprintf(stderr, "[demo] set_remote_description failed\n");
            return;
        }
        pthread_mutex_lock(&d->lock);
        d->remote_set = true;
        /* Flush buffered candidates */
        for (int i = 0; i < d->nb_pend; i++) {
            zstr_webrtc_add_ice_candidate(d->rtc, d->pend_cand[i], d->pend_mid[i]);
            free(d->pend_cand[i]);
            d->pend_cand[i] = NULL;
        }
        d->nb_pend = 0;
        pthread_mutex_unlock(&d->lock);
        const char *answer = zstr_webrtc_create_answer(d->rtc);
        if (!answer) {
            fprintf(stderr, "[demo] create_answer failed\n");
            return;
        }
        zstr_sig_client_send_answer(d->sig, answer);
        printf("[demo] Answer sent, waiting for connection...\n");
    } else if (type == ZSTR_SIG_CANDIDATE) {
        pthread_mutex_lock(&d->lock);
        if (d->remote_set) {
            zstr_webrtc_add_ice_candidate(d->rtc, payload, mid);
        } else if (d->nb_pend < 32) {
            snprintf(d->pend_mid[d->nb_pend], sizeof(d->pend_mid[0]),
                     "%s", mid ? mid : "");
            d->pend_cand[d->nb_pend] = strdup(payload);
            d->nb_pend++;
        }
        pthread_mutex_unlock(&d->lock);
    } else if (type == ZSTR_SIG_MSG) {
        /* App messages from the page: "KEYFRAME <idx>" */
        int idx = -1;
        if (sscanf(payload, "KEYFRAME %d", &idx) == 1 && idx >= 0) {
            printf("[demo] Keyframe requested for track %d\n", idx);
            if (zstr_webrtc_request_keyframe(d->rtc, idx) == 0)
                printf("[demo] Keyframe request sent\n");
            else
                printf("[demo] Keyframe request failed\n");
        } else {
            printf("[demo] MSG: %.120s\n", payload);
        }
    }
}

static void demo_on_dc_message(const char *label, const uint8_t *data,
                               size_t size, bool is_binary, void *ud)
{
    demo_t *d = ud;
    (void)is_binary;
    printf("[demo] DC [%s] got %zu bytes, echoing back\n", label ? label : "?", size);
    if (d && d->rtc && label)
        zstr_webrtc_send_data(d->rtc, label, data, size);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    int port = 8610;
    const char *room = "demo";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--room") == 0 && i + 1 < argc) room = argv[++i];
        else {
            fprintf(stderr, "Usage: %s [--port 8610] [--room demo]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    memset(&g_demo, 0, sizeof(g_demo));
    pthread_mutex_init(&g_demo.lock, NULL);

    zstr_sig_server_t *srv = zstr_sig_server_start(port);
    if (!srv) {
        fprintf(stderr, "[demo] Cannot start signaling server on port %d\n", port);
        return 1;
    }
    int bound = zstr_sig_server_port(srv);
    printf("[demo] Signaling server on ws://127.0.0.1:%d, room '%s'\n", bound, room);

    /* Pure receiver: no local tracks (recv tracks auto-create from offer) */
    g_demo.rtc = zstr_webrtc_alloc("twcc=1");
    if (!g_demo.rtc) {
        fprintf(stderr, "[demo] Engine alloc failed\n");
        return 1;
    }
    zstr_webrtc_set_ice_cb(g_demo.rtc, demo_ice_cb, &g_demo);
    zstr_webrtc_set_dc_message_cb(g_demo.rtc, demo_on_dc_message, &g_demo);

    char url[128];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d", bound);
    g_demo.sig = zstr_sig_client_connect(url, room, demo_on_signal, &g_demo, 10000);
    if (!g_demo.sig) {
        fprintf(stderr, "[demo] Signaling client connect failed\n");
        return 1;
    }
    printf("[demo] Waiting for a browser to publish...\n");
    printf("[demo] Open examples/webrtc_page/index.html in Chrome,\n");
    printf("[demo] signaling URL ws://<this-host>:%d, room '%s'.\n", bound, room);

    AVPacket *pkt = av_packet_alloc();
    int64_t last_stat = now_ms();
    uint64_t last_frames = 0;
    while (g_running) {
        int ti = -1;
        bool got = false;
        int rc = zstr_webrtc_recv_media(g_demo.rtc, pkt, &ti, 500, &got);
        int64_t now = now_ms();
        if (rc == 0 && got) {
            if (ti < 0) ti = 0;
            if (ti > 3) ti = 3;
            g_demo.rx_frames[ti]++;
            g_demo.rx_bytes[ti] += (uint64_t)pkt->size;
            av_packet_unref(pkt);
        }
        if (now - last_stat >= 2000) {
            uint64_t total = g_demo.rx_frames[0] + g_demo.rx_frames[1] +
                             g_demo.rx_frames[2] + g_demo.rx_frames[3];
            uint64_t bytes = g_demo.rx_bytes[0] + g_demo.rx_bytes[1] +
                             g_demo.rx_bytes[2] + g_demo.rx_bytes[3];
            double fps = (total - last_frames) * 1000.0 / (double)(now - last_stat);
            int conn = zstr_webrtc_connected(g_demo.rtc);
            printf("[demo] conn=%d frames=%llu bytes=%llu fps=%.1f\n",
                   conn, (unsigned long long)total, (unsigned long long)bytes, fps);
            char stat[256];
            snprintf(stat, sizeof(stat),
                     "STATS frames=%llu bytes=%llu fps=%.1f connected=%d",
                     (unsigned long long)total, (unsigned long long)bytes, fps, conn);
            zstr_sig_client_send_msg(g_demo.sig, stat);
            last_stat = now;
            last_frames = total;
        }
    }

    printf("\n[demo] Shutting down.\n");
    av_packet_free(&pkt);
    zstr_sig_client_free(&g_demo.sig);
    zstr_webrtc_free(&g_demo.rtc);
    zstr_sig_server_free(&srv);
    pthread_mutex_destroy(&g_demo.lock);
    return 0;
}
