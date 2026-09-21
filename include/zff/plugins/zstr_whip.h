/*=============================================================================
    zstr_whip.h — WHIP (RFC 9725) ingestion client

    Minimal HTTP/1.1 client for WebRTC-HTTP ingestion. Only "http://" URLs
    (no TLS in v1); used by zstr_webrtc_whip_publish() and directly by apps.

    Flow: POST SDP offer -> 201 Created (+ Location + answer SDP),
    optional PATCH trickle-ice, DELETE to terminate.
 =============================================================================*/
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* POST an SDP offer. On 201, returns 0 with malloc'd *answer_sdp and
 * *resource_url (caller frees both). */
int zstr_whip_post_offer(const char *whip_url, const char *offer_sdp,
                         char **answer_sdp, char **resource_url);

/* PATCH one trickle-ice fragment ("a=candidate:..." line, no trailing CRLF
 * required). Returns 0 on 2xx. Best-effort: many endpoints ingest candidates
 * embedded in SDP and never need this. */
int zstr_whip_patch_candidate(const char *resource_url, const char *candidate);

/* DELETE the WHIP resource (end session). Returns 0 on 2xx/404. */
int zstr_whip_delete(const char *resource_url);

#ifdef __cplusplus
}
#endif
