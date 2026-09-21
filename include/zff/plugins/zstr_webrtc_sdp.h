/*=============================================================================
    zstr_webrtc_sdp.h — SDP compatibility surgery for browser interop

    Pure string in/out helpers (malloc'd results, caller frees with free()).
 =============================================================================*/
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Strip extension/rtcp-fb lines libdatachannel cannot negotiate.
 * Keeps transport-wide-cc (feeds TWCC); strips abs-send-time,
 * goog-playout-delay, video-orientation, ssrc-audio-level, and ccm fir
 * (plain PLI is universal; the FIR request path is unreliable). */
char *zstr_sdp_filter(const char *sdp);

/* Restrict each m-section to the best codec per preference list
 * (default video H264>VP8>VP9>H265>AV1, audio opus>PCMU>PCMA>AAC;
 * override with comma-separated "codec_pref", e.g. "VP8,H264").
 * Selected names are copied to video_out/audio_out when non-NULL. */
char *zstr_sdp_select_codecs(const char *sdp, const char *preference,
                             char *selected_video_out, size_t video_out_len,
                             char *selected_audio_out, size_t audio_out_len);

/* Normalize a locally generated SDP for strict peers (Chrome):
 * BUNDLE group, rtcp-mux, msid, ssrc/cname per m-section. */
char *zstr_sdp_compat_local(const char *sdp);

#ifdef __cplusplus
}
#endif
