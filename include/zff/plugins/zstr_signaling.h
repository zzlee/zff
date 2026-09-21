/*=============================================================================
    zstr_signaling.h — Minimal WebSocket signaling for WebRTC

    Transport only (no SDP semantics): a tiny room-routed message switch
    built on libdatachannel's own WebSocket client/server — zstreamer's
    hand-rolled SHA1/handshake server (zst_ws_server.c) is deliberately
    NOT ported.

    Wire protocol (text frames):
      client -> server: "JOIN <room>"
      either -> server: "OFFER <room>\n<sdp>"
                        "ANSWER <room>\n<sdp>"
                        "CANDIDATE <room>\n<mid>\n<candidate>"
      server -> peer:   same verb, routed to the other client(s) in <room>
 =============================================================================*/
#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Server: room message switch --- */
typedef struct zstr_sig_server zstr_sig_server_t;

/** Start a signaling server. port 0 = auto-select (query with port()). */
zstr_sig_server_t *zstr_sig_server_start(int port);
/** Bound port, or -1 on error. */
int zstr_sig_server_port(zstr_sig_server_t *s);
void zstr_sig_server_free(zstr_sig_server_t **s);

/* --- Client --- */
typedef struct zstr_sig_client zstr_sig_client_t;

typedef enum {
    ZSTR_SIG_OFFER = 0,
    ZSTR_SIG_ANSWER,
    ZSTR_SIG_CANDIDATE,
} zstr_sig_msg_t;

/** Incoming routed message. For CANDIDATE, `mid` is set; sdp_or_cand holds
 * the SDP or candidate string (valid only during the callback). */
typedef void (*zstr_sig_message_cb)(zstr_sig_msg_t type, const char *sdp_or_cand,
                                    const char *mid, void *user_data);

/** Connect to url, JOIN room, block until open (timeout_ms) or fail. */
zstr_sig_client_t *zstr_sig_client_connect(const char *url, const char *room,
                                           zstr_sig_message_cb cb, void *user_data,
                                           int timeout_ms);
int zstr_sig_client_send_offer(zstr_sig_client_t *c, const char *sdp);
int zstr_sig_client_send_answer(zstr_sig_client_t *c, const char *sdp);
int zstr_sig_client_send_candidate(zstr_sig_client_t *c, const char *mid,
                                   const char *candidate);
void zstr_sig_client_free(zstr_sig_client_t **c);

#ifdef __cplusplus
}
#endif
