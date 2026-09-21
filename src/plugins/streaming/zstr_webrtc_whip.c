/*=============================================================================
    zstr_webrtc_whip.c — One-call WHIP publish glue for zstr_webrtc
 =============================================================================*/

#include "zff/plugins/zstr_webrtc.h"
#include "zff/plugins/zstr_whip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>

typedef struct {
    char resource[2048];
} whip_trickle_t;

/* Post-gathering candidates trickle out via PATCH (zff profile: body is
 * "mid\ncandidate"; standard sdpfrag is a follow-up). */
static void whip_ice_cb(const char *cand, const char *mid, void *ud)
{
    whip_trickle_t *w = ud;
    char body[2048];
    if (!w || !cand) return;
    snprintf(body, sizeof(body), "%s\n%s", mid ? mid : "", cand);
    zstr_whip_patch_candidate(w->resource, body);
}

#define WHIP_MAX_EARLY_ICE 32

typedef struct {
    char mid[WHIP_MAX_EARLY_ICE][64];
    char cand[WHIP_MAX_EARLY_ICE][512];
    int count;
} whip_early_t;

static void whip_early_cb(const char *cand, const char *mid, void *ud)
{
    whip_early_t *e = ud;
    if (!e || !cand || e->count >= WHIP_MAX_EARLY_ICE) return;
    snprintf(e->mid[e->count], sizeof(e->mid[0]), "%s", mid ? mid : "");
    /* candidates are short (<512) in practice */
    snprintf(e->cand[e->count], sizeof(e->cand[0]), "%s", cand);
    e->count++;
}

int zstr_webrtc_whip_publish(zstr_webrtc_t *s, const char *whip_url,
                             char **resource_url_out, int timeout_ms)
{
    if (!s || !whip_url || timeout_ms <= 0) return AVERROR(EINVAL);
    if (resource_url_out) *resource_url_out = NULL;

    /* Buffer any candidates gathered before the resource URL exists,
     * then POST. (With trickle=0 the offer is already complete and this
     * buffer stays empty; with trickle=1 the offer is early.) */
    whip_early_t early;
    memset(&early, 0, sizeof(early));
    zstr_webrtc_set_ice_cb(s, whip_early_cb, &early);

    /* Offer (candidate-complete when engine trickle=0, early when trickle=1).
     * Copy it: later callbacks may rotate the engine's SDP slot. */
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
    int ret = zstr_whip_post_offer(whip_url, offer_copy, &answer, &resource);
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

    /* Trickle anything gathered from here on, plus the buffered early ones
     * (no-op when gathering already completed before POST). */
    whip_trickle_t w;
    snprintf(w.resource, sizeof(w.resource), "%s", resource);
    for (int i = 0; i < early.count; i++) {
        char body[2048];
        snprintf(body, sizeof(body), "%s\n%s", early.mid[i], early.cand[i]);
        zstr_whip_patch_candidate(w.resource, body);
    }
    zstr_webrtc_set_ice_cb(s, whip_ice_cb, &w);

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
