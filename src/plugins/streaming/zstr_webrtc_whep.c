/*=============================================================================
    zstr_webrtc_whep.c — One-call WHEP playout glue for zstr_webrtc

    Mirror of zstr_webrtc_whip_publish with the direction reversed: the
    local engine offers recvonly (add_recv_*_track first), POSTs to the
    WHEP endpoint, applies the answer, and receives media. The HTTP
    exchange is identical to WHIP (POST offer -> 201 + Location + answer,
    optional PATCH trickle-ice, DELETE to terminate), so it reuses the
    zstr_whip_* transport with a WHEP URL.
 =============================================================================*/

#include "zff/plugins/zstr_webrtc.h"
#include "zff/plugins/zstr_whip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>

typedef struct {
    char resource[2048];
} whep_trickle_t;

/* Post-gathering candidates trickle out via PATCH (same profile as WHIP:
 * body is "mid\ncandidate"). */
static void whep_ice_cb(const char *cand, const char *mid, void *ud)
{
    whep_trickle_t *w = ud;
    char body[2048];
    if (!w || !cand) return;
    snprintf(body, sizeof(body), "%s\n%s", mid ? mid : "", cand);
    zstr_whip_patch_candidate(w->resource, body);
}

#define WHEP_MAX_EARLY_ICE 32

typedef struct {
    char mid[WHEP_MAX_EARLY_ICE][64];
    char cand[WHEP_MAX_EARLY_ICE][512];
    int count;
} whep_early_t;

static void whep_early_cb(const char *cand, const char *mid, void *ud)
{
    whep_early_t *e = ud;
    if (!e || !cand || e->count >= WHEP_MAX_EARLY_ICE) return;
    snprintf(e->mid[e->count], sizeof(e->mid[0]), "%s", mid ? mid : "");
    snprintf(e->cand[e->count], sizeof(e->cand[0]), "%s", cand);
    e->count++;
}

int zstr_webrtc_whep_play(zstr_webrtc_t *s, const char *whep_url,
                          char **resource_url_out, int timeout_ms)
{
    if (!s || !whep_url || timeout_ms <= 0) return AVERROR(EINVAL);
    if (resource_url_out) *resource_url_out = NULL;

    /* Buffer any candidates gathered before the resource URL exists. */
    whep_early_t early;
    memset(&early, 0, sizeof(early));
    zstr_webrtc_set_ice_cb(s, whep_early_cb, &early);

    /* Recvonly offer (caller added recv tracks first). Copy it: later
     * callbacks may rotate the engine's SDP slot. */
    const char *offer = zstr_webrtc_create_offer(s);
    if (!offer) {
        zstr_webrtc_set_ice_cb(s, NULL, NULL);
        return AVERROR(EIO);
    }
    char *offer_copy = strdup(offer);
    if (!offer_copy) {
        zstr_webrtc_set_ice_cb(s, NULL, NULL);
        return AVERROR(ENOMEM);
    }

    char *answer = NULL;
    char *resource = NULL;
    int ret = zstr_whip_post_offer(whep_url, offer_copy, &answer, &resource);
    free(offer_copy);
    if (ret < 0) {
        zstr_webrtc_set_ice_cb(s, NULL, NULL);
        return ret;
    }

    ret = zstr_webrtc_set_remote_description(s, answer, "answer");
    free(answer);
    if (ret < 0) {
        free(resource);
        zstr_webrtc_set_ice_cb(s, NULL, NULL);
        return ret;
    }

    /* Trickle anything gathered from here on, plus buffered early ones. */
    whep_trickle_t w;
    snprintf(w.resource, sizeof(w.resource), "%s", resource);
    for (int i = 0; i < early.count; i++) {
        char body[2048];
        snprintf(body, sizeof(body), "%s\n%s", early.mid[i], early.cand[i]);
        zstr_whip_patch_candidate(w.resource, body);
    }
    zstr_webrtc_set_ice_cb(s, whep_ice_cb, &w);

    ret = zstr_webrtc_wait_connected(s, timeout_ms);

    zstr_webrtc_set_ice_cb(s, NULL, NULL);
    if (ret < 0) {
        free(resource);
        return ret;
    }

    if (resource_url_out) {
        *resource_url_out = resource;
    } else {
        free(resource);
    }
    return 0;
}
