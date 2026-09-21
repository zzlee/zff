/*=============================================================================

    zstr_webrtc_signaling.c — One-call WebRTC handshake over zstr_signaling

    Replaces file-based SDP exchange (and zstreamer's manual demo wiring)

    with a real signaling round-trip:

      offerer:  OFFER -> wait ANSWER -> set_remote -> ICE -> connected

      answerer: wait OFFER -> set_remote -> ANSWER -> ICE -> connected

    Remote ICE candidates arriving before set_remote_description are

    buffered and flushed after (libdatachannel rejects early candidates).

 =============================================================================*/

#include "zff/plugins/zstr_webrtc.h"

#include "zff/plugins/zstr_signaling.h"

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <pthread.h>

#include <time.h>

#include <libavutil/error.h>

#define HS_MAX_PENDING_ICE 32

typedef struct {

    zstr_webrtc_t *rtc;

    zstr_sig_client_t *sig;

    pthread_mutex_t lock;

    pthread_cond_t cond;

    char *remote_offer;

    char *remote_answer;

    struct {

        char mid[64];

        char *cand; /* strdup'd */

    } pending[HS_MAX_PENDING_ICE];

    int nb_pending;

    bool remote_set;

    bool failed;

} handshake_t;

static int64_t now_ms(void)

{

    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

}

static void hs_on_ice(const char *cand, const char *mid, void *ud)

{

    handshake_t *h = ud;

    if (!h || !cand) return;

    zstr_sig_client_send_candidate(h->sig, mid ? mid : "", cand);

}

static void hs_flush_pending(handshake_t *h)

{

    for (int i = 0; i < h->nb_pending; i++) {

        zstr_webrtc_add_ice_candidate(h->rtc, h->pending[i].cand, h->pending[i].mid);

        free(h->pending[i].cand);

        h->pending[i].cand = NULL;

    }

    h->nb_pending = 0;

}

static void hs_on_signal(zstr_sig_msg_t type, const char *payload,

                         const char *mid, void *ud)

{

    handshake_t *h = ud;

    if (!h || !payload) return;

    pthread_mutex_lock(&h->lock);

    if (type == ZSTR_SIG_OFFER) {

        free(h->remote_offer);

        h->remote_offer = strdup(payload);

        pthread_cond_signal(&h->cond);

    } else if (type == ZSTR_SIG_ANSWER) {

        free(h->remote_answer);

        h->remote_answer = strdup(payload);

        pthread_cond_signal(&h->cond);

    } else if (type == ZSTR_SIG_CANDIDATE) {

        if (h->remote_set) {

            zstr_webrtc_add_ice_candidate(h->rtc, payload, mid);

        } else if (h->nb_pending < HS_MAX_PENDING_ICE) {

            snprintf(h->pending[h->nb_pending].mid,

                     sizeof(h->pending[0].mid), "%s", mid ? mid : "");

            h->pending[h->nb_pending].cand = strdup(payload);

            h->nb_pending++;

        }

    }

    pthread_mutex_unlock(&h->lock);

}

/* Wait until *flag_str is non-NULL (copies it) or deadline passes. */

static char *hs_wait_str(handshake_t *h, char **field, int64_t deadline)

{

    pthread_mutex_lock(&h->lock);

    while (!*field && !h->failed) {

        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);

        int64_t left = deadline - now_ms();

        if (left <= 0) break;

        ts.tv_sec += left / 1000;

        ts.tv_nsec += (left % 1000) * 1000000L;

        if (ts.tv_nsec >= 1000000000L) {

            ts.tv_sec++;

            ts.tv_nsec -= 1000000000L;

        }

        pthread_cond_timedwait(&h->cond, &h->lock, &ts);

    }

    char *out = NULL;

    if (*field) {

        out = *field;

        *field = NULL;

    }

    pthread_mutex_unlock(&h->lock);

    return out;

}

int zstr_webrtc_connect_signaling(zstr_webrtc_t *s, const char *url,

                                  const char *room, bool is_offerer,

                                  int timeout_ms)

{

    if (!s || !url || !room || timeout_ms <= 0) return -1;

    int64_t deadline = now_ms() + timeout_ms;

    handshake_t h;

    memset(&h, 0, sizeof(h));

    h.rtc = s;

    pthread_mutex_init(&h.lock, NULL);

    pthread_cond_init(&h.cond, NULL);

    h.sig = zstr_sig_client_connect(url, room, hs_on_signal, &h,

                                    (int)(deadline - now_ms()));

    if (!h.sig) goto fail;

    zstr_webrtc_set_ice_cb(s, hs_on_ice, &h);

    if (is_offerer) {

        const char *offer = zstr_webrtc_create_offer(s);

        if (!offer) goto fail;

        if (zstr_sig_client_send_offer(h.sig, offer) < 0) goto fail;

        char *answer = hs_wait_str(&h, &h.remote_answer, deadline);

        if (!answer) goto fail;

        if (zstr_webrtc_set_remote_description(s, answer, "answer") < 0) {

            free(answer);

            goto fail;

        }

        free(answer);

    } else {

        char *offer = hs_wait_str(&h, &h.remote_offer, deadline);

        if (!offer) goto fail;

        if (zstr_webrtc_set_remote_description(s, offer, "offer") < 0) {

            free(offer);

            goto fail;

        }

        free(offer);

        const char *answer = zstr_webrtc_create_answer(s);

        if (!answer) goto fail;

        if (zstr_sig_client_send_answer(h.sig, answer) < 0) goto fail;

    }

    /* Remote description is set: flush buffered ICE, then connect. */

    pthread_mutex_lock(&h.lock);

    h.remote_set = true;

    hs_flush_pending(&h);

    pthread_mutex_unlock(&h.lock);

    int64_t left = deadline - now_ms();

    int ret = zstr_webrtc_wait_connected(s, left > 0 ? (int)left : 0);

    zstr_webrtc_set_ice_cb(s, NULL, NULL);

    zstr_sig_client_free(&h.sig);

    free(h.remote_offer);

    free(h.remote_answer);

    for (int i = 0; i < h.nb_pending; i++) free(h.pending[i].cand);

    pthread_mutex_destroy(&h.lock);

    pthread_cond_destroy(&h.cond);

    return ret;

fail:

    zstr_webrtc_set_ice_cb(s, NULL, NULL);

    if (h.sig) zstr_sig_client_free(&h.sig);

    free(h.remote_offer);

    free(h.remote_answer);

    for (int i = 0; i < h.nb_pending; i++) free(h.pending[i].cand);

    pthread_mutex_destroy(&h.lock);

    pthread_cond_destroy(&h.cond);

    return -1;

}
