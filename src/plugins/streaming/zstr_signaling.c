/*=============================================================================

    zstr_signaling.c — Room-routed WebSocket signaling switch + client

    See zstr_signaling.h for the wire protocol. Threading: libdatachannel

    invokes callbacks on its own threads; all shared state is mutex-held

    and callbacks never call back into blocking engine functions.

 =============================================================================*/

#define _GNU_SOURCE

#include "zff/plugins/zstr_signaling.h"

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <pthread.h>

#include <time.h>

#include <rtc/rtc.h>

#include <libavutil/log.h>

#include <libavutil/error.h>

#define SIG_MAX_ROOMS 16

#define SIG_MAX_CLIENTS_PER_ROOM 4

#define SIG_ROOM_LEN 64

#define SIG_MID_LEN 64

/* --- Server --- */

typedef struct {

    int ws;

    char room[SIG_ROOM_LEN];

    bool joined;

} sig_member_t;

struct zstr_sig_server {

    int wsserver;

    pthread_mutex_t lock;

    sig_member_t members[SIG_MAX_ROOMS * SIG_MAX_CLIENTS_PER_ROOM];

};

static sig_member_t *server_find(struct zstr_sig_server *s, int ws)

{

    for (size_t i = 0; i < sizeof(s->members) / sizeof(s->members[0]); i++) {

        if (s->members[i].ws == ws) return &s->members[i];

    }

    return NULL;

}

static void server_route(struct zstr_sig_server *s, int from_ws,

                         const char *msg, int size)

{

    /* Parse "VERB <room>\n<rest>" — route raw bytes to joined peers */

    char verb[16] = "";

    char room[SIG_ROOM_LEN] = "";

    if (sscanf(msg, "%15s %63s", verb, room) != 2) return;

    if (strcmp(verb, "OFFER") != 0 && strcmp(verb, "ANSWER") != 0 &&

        strcmp(verb, "CANDIDATE") != 0)

        return;

    pthread_mutex_lock(&s->lock);

    sig_member_t *from = server_find(s, from_ws);

    if (from && !from->joined) {

        /* Implicit join: adopt the room of the first signaling message */

        snprintf(from->room, sizeof(from->room), "%s", room);

        from->joined = true;

    }

    for (size_t i = 0; i < sizeof(s->members) / sizeof(s->members[0]); i++) {

        sig_member_t *m = &s->members[i];

        if (m->ws >= 0 && m->ws != from_ws && m->joined &&

            strcmp(m->room, room) == 0) {

            rtcSendMessage(m->ws, msg, size);

        }

    }

    pthread_mutex_unlock(&s->lock);

}

static void server_on_message(int ws, const char *message, int size, void *ptr)

{

    struct zstr_sig_server *s = ptr;

    /* NOTE: text frames carry negative size (libdatachannel convention:

     * -(bytes including NUL)); only size == 0 means empty. */

    if (!s || !message || size == 0) return;

    /* libdatachannel uses negative size for text strings */

    const char *msg = message;

    int len = size < 0 ? -size : size;

    char verb[16] = "";

    if (sscanf(msg, "%15s", verb) != 1) return;

    if (strcmp(verb, "JOIN") == 0) {

        char room[SIG_ROOM_LEN] = "";

        if (sscanf(msg, "%*s %63s", room) != 1) return;

        pthread_mutex_lock(&s->lock);

        sig_member_t *m = server_find(s, ws);

        if (m) {

            snprintf(m->room, sizeof(m->room), "%s", room);

            m->joined = true;

        }

        pthread_mutex_unlock(&s->lock);

        return;

    }

    server_route(s, ws, msg, len);

}

static void server_on_client(int wsserver, int ws, void *ptr)

{

    (void)wsserver;

    struct zstr_sig_server *s = ptr;

    if (!s) return;

    pthread_mutex_lock(&s->lock);

    sig_member_t *slot = NULL;

    for (size_t i = 0; i < sizeof(s->members) / sizeof(s->members[0]); i++) {

        if (s->members[i].ws < 0 && !slot) slot = &s->members[i];

    }

    if (slot) {

        slot->ws = ws;

        slot->joined = false;

        slot->room[0] = '\0';

        rtcSetUserPointer(ws, s);

        int smr = rtcSetMessageCallback(ws, server_on_message);

    }

    pthread_mutex_unlock(&s->lock);

    if (!slot) rtcDeleteWebSocket(ws); /* room table full: refuse */

}

zstr_sig_server_t *zstr_sig_server_start(int port)

{

    zstr_sig_server_t *s = calloc(1, sizeof(*s));

    if (!s) return NULL;

    for (size_t i = 0; i < sizeof(s->members) / sizeof(s->members[0]); i++)

        s->members[i].ws = -1;

    pthread_mutex_init(&s->lock, NULL);

    rtcWsServerConfiguration cfg = { 0 };

    cfg.port = (uint16_t)(port > 0 ? port : 0);

    cfg.bindAddress = "127.0.0.1";

    s->wsserver = rtcCreateWebSocketServer(&cfg, server_on_client);

    if (s->wsserver < 0) {

        pthread_mutex_destroy(&s->lock);

        free(s);

        return NULL;

    }

    rtcSetUserPointer(s->wsserver, s);

    return s;

}

int zstr_sig_server_port(zstr_sig_server_t *s)

{

    if (!s) return -1;

    return rtcGetWebSocketServerPort(s->wsserver);

}

void zstr_sig_server_free(zstr_sig_server_t **ps)

{

    if (!ps || !*ps) return;

    zstr_sig_server_t *s = *ps;

    rtcDeleteWebSocketServer(s->wsserver);

    pthread_mutex_destroy(&s->lock);

    free(s);

    *ps = NULL;

}

/* --- Client --- */

struct zstr_sig_client {

    int ws;

    char room[SIG_ROOM_LEN];

    zstr_sig_message_cb cb;

    void *cb_ud;

    pthread_mutex_t lock;

    pthread_cond_t open_cond;

    bool open;

    bool failed;

};

static void client_on_open(int ws, void *ptr)

{

    (void)ws;

    zstr_sig_client_t *c = ptr;

    if (!c) return;

    pthread_mutex_lock(&c->lock);

    c->open = true;

    pthread_cond_signal(&c->open_cond);

    pthread_mutex_unlock(&c->lock);

    /* JOIN immediately so the server can route to us */

    char join[SIG_ROOM_LEN + 8];

    snprintf(join, sizeof(join), "JOIN %s", c->room);

    int jr = rtcSendMessage(c->ws, join, -(int)(strlen(join) + 1));

}

static void client_on_closed(int ws, void *ptr)

{

    (void)ws;

    zstr_sig_client_t *c = ptr;

    if (!c) return;

    pthread_mutex_lock(&c->lock);

    c->failed = true;

    pthread_cond_signal(&c->open_cond);

    pthread_mutex_unlock(&c->lock);

}

static void client_on_message(int ws, const char *message, int size, void *ptr)

{

    (void)ws;

    zstr_sig_client_t *c = ptr;

    if (!c || !message || size == 0) return;

    /* Text frames arrive with negative size; copy to NUL-terminate */

    int len = size < 0 ? -size : size;

    char *copy = malloc(len + 1);

    if (!copy) return;

    memcpy(copy, message, len);

    copy[len] = '\0';

    char verb[16] = "";

    zstr_sig_msg_t type;

    const char *body = NULL;

    char mid[SIG_MID_LEN] = "";

    if (sscanf(copy, "%15s", verb) != 1) {

        free(copy);

        return;

    }

    if (strcmp(verb, "OFFER") == 0) {

        type = ZSTR_SIG_OFFER;

        body = strchr(copy, '\n');

        body = body ? body + 1 : "";

    } else if (strcmp(verb, "ANSWER") == 0) {

        type = ZSTR_SIG_ANSWER;

        body = strchr(copy, '\n');

        body = body ? body + 1 : "";

    } else if (strcmp(verb, "CANDIDATE") == 0) {

        type = ZSTR_SIG_CANDIDATE;

        /* "CANDIDATE <room>\n<mid>\n<candidate>" */

        const char *l1 = strchr(copy, '\n');

        if (!l1) {

            free(copy);

            return;

        }

        const char *l2 = strchr(l1 + 1, '\n');

        if (!l2) {

            free(copy);

            return;

        }

        size_t mlen = (size_t)(l2 - (l1 + 1));

        if (mlen >= sizeof(mid)) mlen = sizeof(mid) - 1;

        memcpy(mid, l1 + 1, mlen);

        mid[mlen] = '\0';

        body = l2 + 1;

    } else {

        free(copy);

        return;

    }

    zstr_sig_message_cb cb;

    void *ud;

    pthread_mutex_lock(&c->lock);

    cb = c->cb;

    ud = c->cb_ud;

    pthread_mutex_unlock(&c->lock);

    if (cb) cb(type, body, mid, ud);

    free(copy);

}

zstr_sig_client_t *zstr_sig_client_connect(const char *url, const char *room,

                                           zstr_sig_message_cb cb, void *user_data,

                                           int timeout_ms)

{

    if (!url || !room) return NULL;

    zstr_sig_client_t *c = calloc(1, sizeof(*c));

    if (!c) return NULL;

    snprintf(c->room, sizeof(c->room), "%s", room);

    c->cb = cb;

    c->cb_ud = user_data;

    pthread_mutex_init(&c->lock, NULL);

    pthread_cond_init(&c->open_cond, NULL);

    c->ws = rtcCreateWebSocket(url);

    if (c->ws < 0) goto fail;

    rtcSetUserPointer(c->ws, c);

    rtcSetOpenCallback(c->ws, client_on_open);

    rtcSetClosedCallback(c->ws, client_on_closed);

    rtcSetMessageCallback(c->ws, client_on_message);

    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    ts.tv_sec += timeout_ms / 1000;

    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;

    if (ts.tv_nsec >= 1000000000L) {

        ts.tv_sec++;

        ts.tv_nsec -= 1000000000L;

    }

    pthread_mutex_lock(&c->lock);

    while (!c->open && !c->failed) {

        if (pthread_cond_timedwait(&c->open_cond, &c->lock, &ts) == ETIMEDOUT) break;

    }

    bool ok = c->open;

    pthread_mutex_unlock(&c->lock);

    if (!ok) goto fail;

    return c;

fail:

    if (c->ws >= 0) rtcDeleteWebSocket(c->ws);

    pthread_mutex_destroy(&c->lock);

    pthread_cond_destroy(&c->open_cond);

    free(c);

    return NULL;

}

static int client_send(zstr_sig_client_t *c, const char *data)

{

    if (!c || !data) return -1;

    /* Text frame: negative size includes NUL (libdatachannel convention) */

    int ret = rtcSendMessage(c->ws, data, -(int)(strlen(data) + 1));

    return ret >= 0 ? 0 : -1;

}

int zstr_sig_client_send_offer(zstr_sig_client_t *c, const char *sdp)

{

    if (!c || !sdp) return -1;

    size_t cap = strlen(sdp) + 256;

    char *buf = malloc(cap);

    if (!buf) return -1;

    snprintf(buf, cap, "OFFER %s\n%s", c->room, sdp);

    int ret = client_send(c, buf);

    free(buf);

    return ret;

}

int zstr_sig_client_send_answer(zstr_sig_client_t *c, const char *sdp)

{

    if (!c || !sdp) return -1;

    size_t cap = strlen(sdp) + 256;

    char *buf = malloc(cap);

    if (!buf) return -1;

    snprintf(buf, cap, "ANSWER %s\n%s", c->room, sdp);

    int ret = client_send(c, buf);

    free(buf);

    return ret;

}

int zstr_sig_client_send_candidate(zstr_sig_client_t *c, const char *mid,

                                   const char *candidate)

{

    if (!c || !candidate) return -1;

    size_t cap = strlen(candidate) + 256;

    char *buf = malloc(cap);

    if (!buf) return -1;

    snprintf(buf, cap, "CANDIDATE %s\n%s\n%s", c->room, mid ? mid : "", candidate);

    int ret = client_send(c, buf);

    free(buf);

    return ret;

}

void zstr_sig_client_free(zstr_sig_client_t **pc)

{

    if (!pc || !*pc) return;

    zstr_sig_client_t *c = *pc;

    if (c->ws >= 0) {

        rtcDeleteWebSocket(c->ws);

        c->ws = -1;

    }

    pthread_mutex_destroy(&c->lock);

    pthread_cond_destroy(&c->open_cond);

    free(c);

    *pc = NULL;

}
