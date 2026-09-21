/*=============================================================================
    test_webrtc_whip.c — WHIP publish against an in-process stub server

    Stub WHIP endpoint (plain POSIX HTTP): POST offer -> answerer engine
    answers -> 201 + Location + answer; PATCH forwards trickle candidates;
    DELETE ends the session. Publisher uses trickle=1 (exercises PATCH);
    answerer uses trickle=0 (complete answer). Media round-trips.
 =============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <libavcodec/packet.h>
#include <libavutil/error.h>

#include "zff/plugins/zstr_webrtc.h"
#include "zff/plugins/zstr_whip.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

typedef struct {
    int listen_fd;
    int port;
    volatile int running;
    pthread_t thread;
    zstr_webrtc_t *answerer; /* owned by main, driven here on POST/PATCH */
    volatile int patch_count;
    volatile int delete_count;
} whip_stub_t;

static int stub_send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void stub_reply(int fd, int code, const char *reason,
                       const char *extra_hdrs, const char *body, size_t body_len)
{
    char hdr[2048];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/sdp\r\n"
                     "Content-Length: %zu\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     code, reason, body_len, extra_hdrs ? extra_hdrs : "");
    stub_send_all(fd, hdr, (size_t)n);
    if (body && body_len) stub_send_all(fd, body, body_len);
}

/* Read a full HTTP request (headers + Content-Length body). */
static int stub_read_request(int fd, char *method, size_t method_cap,
                             char *path, size_t path_cap,
                             char **body_out, size_t *body_len_out)
{
    static char buf[256 * 1024];
    size_t total = 0;
    size_t content_len = 0;
    int have_headers = 0;
    size_t hdr_len = 0;

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 5000) <= 0) return -1;
        ssize_t n = recv(fd, buf + total, sizeof(buf) - total - 1, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';

        if (!have_headers) {
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) {
                if (total >= sizeof(buf) - 1) return -1;
                continue;
            }
            hdr_len = (size_t)(end - buf) + 4;
            have_headers = 1;
            if (sscanf(buf, "%15s %1023s", method, path) != 2) return -1;
            char *cl = strcasestr(buf, "Content-Length:");
            if (cl && cl < end) content_len = (size_t)atol(cl + 15);
        }
        if (have_headers && total >= hdr_len + content_len) break;
    }

    /* method/path buffers sized by caller (16/1024) */
    (void)method_cap;
    (void)path_cap;
    if (content_len > 0) {
        *body_out = malloc(content_len + 1);
        if (!*body_out) return -1;
        memcpy(*body_out, buf + hdr_len, content_len);
        (*body_out)[content_len] = '\0';
        *body_len_out = content_len;
    } else {
        *body_out = NULL;
        *body_len_out = 0;
    }
    return 0;
}

static void *stub_thread(void *arg)
{
    whip_stub_t *st = arg;
    while (st->running) {
        struct pollfd pfd = { .fd = st->listen_fd, .events = POLLIN };
        if (poll(&pfd, 1, 200) <= 0) continue;

        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(st->listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) continue;

        char method[16] = "", path[1024] = "";
        char *body = NULL;
        size_t body_len = 0;
        if (stub_read_request(fd, method, sizeof(method), path, sizeof(path),
                              &body, &body_len) < 0) {
            close(fd);
            free(body);
            continue;
        }

        if (strcmp(method, "POST") == 0) {
            /* Offer -> answer via the answerer engine */
            int rc = zstr_webrtc_set_remote_description(st->answerer,
                                                        body ? body : "", "offer");
            const char *answer = NULL;
            if (rc == 0) answer = zstr_webrtc_create_answer(st->answerer);
            if (rc == 0 && answer)
                answer = zstr_webrtc_complete_gathering(st->answerer, 15000);
            if (rc == 0 && answer) {
                char loc[128];
                snprintf(loc, sizeof(loc), "Location: /whip/sess1\r\n");
                stub_reply(fd, 201, "Created", loc, answer, strlen(answer));
            } else {
                stub_reply(fd, 500, "Internal Error", NULL, NULL, 0);
            }
        } else if (strcmp(method, "PATCH") == 0) {
            /* zff profile body: "mid\\ncandidate" */
            char mid[64] = "";
            const char *cand = body ? body : "";
            const char *nl = body ? strchr(body, '\n') : NULL;
            if (nl) {
                size_t mlen = (size_t)(nl - body);
                if (mlen >= sizeof(mid)) mlen = sizeof(mid) - 1;
                memcpy(mid, body, mlen);
                mid[mlen] = '\0';
                cand = nl + 1;
            }
            if (zstr_webrtc_add_ice_candidate(st->answerer, cand, mid) == 0)
                st->patch_count++;
            stub_reply(fd, 204, "No Content", NULL, NULL, 0);
        } else if (strcmp(method, "DELETE") == 0) {
            st->delete_count++;
            stub_reply(fd, 200, "OK", NULL, NULL, 0);
        } else {
            stub_reply(fd, 405, "Method Not Allowed", NULL, NULL, 0);
        }
        free(body);
        close(fd);
    }
    return NULL;
}

static void test_whip_publish(void)
{
    printf("[TEST] WHIP publish against stub server (trickle publisher)...\n");

    /* Answerer engine (pure receiver, complete answers) */
    zstr_webrtc_t *b = zstr_webrtc_alloc("twcc=1:trickle=0");
    CHECK(b != NULL);

    whip_stub_t stub;
    memset(&stub, 0, sizeof(stub));
    stub.answerer = b;
    stub.running = 1;
    stub.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(stub.listen_fd >= 0);
    int reuse = 1;
    setsockopt(stub.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(stub.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(stub.listen_fd, 8) == 0);
    socklen_t alen = sizeof(addr);
    CHECK(getsockname(stub.listen_fd, (struct sockaddr *)&addr, &alen) == 0);
    stub.port = ntohs(addr.sin_port);
    CHECK(pthread_create(&stub.thread, NULL, stub_thread, &stub) == 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/whip", stub.port);

    /* Publisher with trickle=1 (tests PATCH path when candidates lag) */
    zstr_webrtc_t *a = zstr_webrtc_alloc("twcc=1:trickle=1");
    CHECK(a != NULL);
    CHECK(zstr_webrtc_add_video_track(a, ZSTR_WEBRTC_CODEC_H264, 96, 90000) == 0);
    CHECK(zstr_webrtc_add_audio_track(a, ZSTR_WEBRTC_CODEC_OPUS, 111, 48000) == 1);

    char *resource = NULL;
    CHECK(zstr_webrtc_whip_publish(a, url, &resource, 25000) == 0);
    CHECK(resource != NULL && resource[0] != '\0');
    printf("[INFO] Published, resource %s, server saw %d PATCH(es).\n",
           resource, stub.patch_count);

    /* Media A -> B */
    uint8_t h264[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22, 0x33 };
    for (int i = 0; i < 3; i++) {
        AVPacket *pkt = av_packet_alloc();
        av_new_packet(pkt, sizeof(h264));
        memcpy(pkt->data, h264, sizeof(h264));
        pkt->stream_index = 0;
        pkt->pts = i * 3000;
        pkt->time_base = (AVRational){ 1, 90000 };
        CHECK(zstr_webrtc_send_media(a, 0, pkt) == 0);
        av_packet_free(&pkt);
    }
    uint8_t opus[16];
    memset(opus, 0xCC, sizeof(opus));
    for (int i = 0; i < 3; i++) {
        AVPacket *pkt = av_packet_alloc();
        av_new_packet(pkt, sizeof(opus));
        memcpy(pkt->data, opus, sizeof(opus));
        pkt->stream_index = 1;
        pkt->pts = i * 960;
        pkt->time_base = (AVRational){ 1, 48000 };
        CHECK(zstr_webrtc_send_media(a, 1, pkt) == 0);
        av_packet_free(&pkt);
    }

    int got_video = 0, got_audio = 0;
    AVPacket *rx = av_packet_alloc();
    for (int i = 0; i < 60 && (got_video < 3 || got_audio < 3); i++) {
        int ti = -1;
        bool got = false;
        int rc = zstr_webrtc_recv_media(b, rx, &ti, 200, &got);
        if (rc == AVERROR(ETIMEDOUT)) continue;
        CHECK(rc == 0 && got);
        if (rx->size == 8 && rx->data[4] == 0x65) got_video++;
        else if (rx->size == 16 && rx->data[0] == 0xCC) got_audio++;
        av_packet_unref(rx);
    }
    av_packet_free(&rx);
    CHECK(got_video == 3);
    CHECK(got_audio == 3);

    /* DELETE terminates the session */
    CHECK(zstr_whip_delete(resource) == 0);
    CHECK(stub.delete_count >= 1);
    free(resource);

    stub.running = 0;
    shutdown(stub.listen_fd, SHUT_RDWR);
    pthread_join(stub.thread, NULL);
    close(stub.listen_fd);
    zstr_webrtc_free(&a);
    zstr_webrtc_free(&b);

    printf("[PASS] WHIP publish loopback passed (3 video + 3 audio).\n");
}

int main(void)
{
    printf("====================================================\n");
    printf("         Running zstr WHIP Publish Tests            \n");
    printf("====================================================\n");

    test_whip_publish();

    printf("====================================================\n");
    printf("      All zstr WHIP Tests Passed!                   \n");
    printf("====================================================\n");
    return 0;
}
