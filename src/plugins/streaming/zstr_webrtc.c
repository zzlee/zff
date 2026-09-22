/*=============================================================================
    zstr_webrtc.c — WebRTC PeerConnection Engine (libdatachannel)

    Ported from zstreamer `webrtc_endpoint.c` (phases 1-10). Framework deltas:
    - zst_element/pads/bus -> plain C engine; media flows as AVPacket.
    - Received frames land in a thread-safe FIFO drained by recv_media().
    - TWCC interceptors (patched libdatachannel) feed zstr_webrtc_twcc;
      bitrate estimates surface through the app callback.
    - Chrome-interop SDP surgery (filter/select/compat) is intentionally
      deferred: libdatachannel negotiates H264/VP8/VP9/Opus cleanly
      peer-to-peer, which is all v1 needs. See PORTING_ANALYSIS for the
      follow-up list.
 =============================================================================*/

#include "zff/plugins/zstr_webrtc.h"
#include "zff/plugins/zstr_webrtc_twcc.h"
#include "zff/plugins/zstr_webrtc_sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <rtc/rtc.h>

#include <libavutil/log.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#define ZSTR_WEBRTC_MAX_TRACKS 4
#define ZSTR_WEBRTC_MAX_DC     8
#define ZSTR_WEBRTC_MAX_ICE    8
#define ZSTR_WEBRTC_SDP_CAP    16384
#define ZSTR_WEBRTC_RX_CAP     64

#define LOG(...) av_log(NULL, AV_LOG_INFO, "zstr_webrtc: " __VA_ARGS__)

typedef struct {
    zstr_webrtc_codec_t codec;
    uint32_t clock_rate;
    uint32_t ssrc;
    uint8_t payload_type;
    char mid[32];
    int track_id; /* libdatachannel handle, -1 when not created */
    bool is_audio;
} webrtc_track_t;

typedef struct {
    AVPacket *pkt;
    int track_idx;
} rx_item_t;

struct zstr_webrtc {
    /* Config */
    char *ice_servers[ZSTR_WEBRTC_MAX_ICE];
    int nb_ice_servers;
    bool twcc_enable;

    /* SDP compat */
    char *codec_pref; /* comma-separated codec preference, NULL = defaults */
    char selected_video[32];
    char selected_audio[32];
    /* Offered section MIDs from the last remote offer (for answer tracks) */
    char offered_video_mid[32];
    char offered_audio_mid[32];

    /* libdatachannel state */
    int pc_id;
    bool pc_created;
    rtcState conn_state;
    rtcGatheringState gathering_state;
    bool trickle; /* 1 = trickle ICE via ice_cb (default); 0 = wait for
                     complete gathering so SDPs are self-contained */

    /* Local SDP (owned, signaled on generation). Two rotating slots so a
     * previously returned pointer survives one subsequent renegotiation. */
    char local_sdp[2][ZSTR_WEBRTC_SDP_CAP];
    int sdp_slot;
    char local_type[16];
    pthread_mutex_t sdp_lock;
    pthread_cond_t sdp_cond;
    uint64_t sdp_gen;   /* bumped on every local description */

    /* Signaling serialization */
    pthread_mutex_t sig_lock;

    /* Tracks */
    webrtc_track_t tracks[ZSTR_WEBRTC_MAX_TRACKS];
    int nb_tracks;

    /* Receive FIFO */
    rx_item_t rx[ZSTR_WEBRTC_RX_CAP];
    int rx_head, rx_tail, rx_count;
    pthread_mutex_t rx_lock;
    pthread_cond_t rx_cond;

    /* Data channels */
    struct {
        char label[64];
        int dc_id;
        bool open;
    } dcs[ZSTR_WEBRTC_MAX_DC];
    int nb_dcs;

    /* Diagnostics: PSFB/PLI packets seen at the PC-level tap */
    uint64_t pli_rx_count;

    /* Inbound (remote-created) tracks: handle -> codec clock for PTS */
    struct {
        int track_id;
        uint32_t clock_rate;
        bool active;
    } recv[ZSTR_WEBRTC_MAX_TRACKS];
    int nb_recv;

    /* App callbacks */
    zstr_webrtc_bitrate_cb bitrate_cb;
    void *bitrate_ud;
    zstr_webrtc_keyframe_cb keyframe_cb;
    void *keyframe_ud;
    zstr_webrtc_ice_cb ice_cb;
    void *ice_ud;
    zstr_webrtc_dc_message_cb dc_cb;
    void *dc_ud;

    /* TWCC */
    zstr_webrtc_twcc_t *twcc;
};

/* --- Codec helpers (from zstreamer) --- */
static rtcCodec codec_to_rtc(zstr_webrtc_codec_t c)
{
    switch (c) {
        case ZSTR_WEBRTC_CODEC_H264: return RTC_CODEC_H264;
        case ZSTR_WEBRTC_CODEC_H265: return RTC_CODEC_H265;
        case ZSTR_WEBRTC_CODEC_VP8:  return RTC_CODEC_VP8;
        case ZSTR_WEBRTC_CODEC_VP9:  return RTC_CODEC_VP9;
        case ZSTR_WEBRTC_CODEC_OPUS: return RTC_CODEC_OPUS;
        case ZSTR_WEBRTC_CODEC_PCMU: return RTC_CODEC_PCMU;
        case ZSTR_WEBRTC_CODEC_PCMA: return RTC_CODEC_PCMA;
        case ZSTR_WEBRTC_CODEC_AAC:  return RTC_CODEC_AAC;
        default:                     return RTC_CODEC_H264;
    }
}

static uint32_t codec_clock_rate(zstr_webrtc_codec_t c)
{
    switch (c) {
        case ZSTR_WEBRTC_CODEC_OPUS:
        case ZSTR_WEBRTC_CODEC_AAC:  return 48000;
        case ZSTR_WEBRTC_CODEC_PCMU:
        case ZSTR_WEBRTC_CODEC_PCMA: return 8000;
        default:                     return 90000;
    }
}

static bool codec_is_audio(zstr_webrtc_codec_t c)
{
    return c == ZSTR_WEBRTC_CODEC_OPUS || c == ZSTR_WEBRTC_CODEC_PCMU ||
           c == ZSTR_WEBRTC_CODEC_PCMA || c == ZSTR_WEBRTC_CODEC_AAC;
}

static zstr_webrtc_codec_t codec_from_sdp(const char *sdp)
{
    if (!sdp) return ZSTR_WEBRTC_CODEC_H264;
    if (strstr(sdp, "H264") || strstr(sdp, "h264")) return ZSTR_WEBRTC_CODEC_H264;
    if (strstr(sdp, "H265") || strstr(sdp, "h265")) return ZSTR_WEBRTC_CODEC_H265;
    if (strstr(sdp, "VP8")  || strstr(sdp, "vp8"))  return ZSTR_WEBRTC_CODEC_VP8;
    if (strstr(sdp, "VP9")  || strstr(sdp, "vp9"))  return ZSTR_WEBRTC_CODEC_VP9;
    if (strstr(sdp, "opus") || strstr(sdp, "OPUS")) return ZSTR_WEBRTC_CODEC_OPUS;
    return ZSTR_WEBRTC_CODEC_H264;
}

static void on_pli_shim(int tr, void *ptr)
{
    zstr_webrtc_t *s = ptr;
    if (!s) return;
    /* Resolve media index from send tracks, then recv table */
    int idx = -1;
    pthread_mutex_lock(&s->sig_lock);
    for (int i = 0; i < s->nb_tracks && idx < 0; i++) {
        if (s->tracks[i].track_id == tr) idx = i;
    }
    for (int i = 0; i < s->nb_recv && idx < 0; i++) {
        if (s->recv[i].active && s->recv[i].track_id == tr) idx = i;
    }
    zstr_webrtc_keyframe_cb cb = s->keyframe_cb;
    void *ud = s->keyframe_ud;
    pthread_mutex_unlock(&s->sig_lock);
    if (cb && idx >= 0) cb(idx, ud);
}

static void on_remb_shim(int tr, unsigned int bitrate, void *ptr)
{
    (void)tr;
    zstr_webrtc_t *s = ptr;
    if (!s) return;
    /* Receiver-estimated bitrate is a direct congestion signal: surface it
     * through the same application callback as the GCC estimate. */
    pthread_mutex_lock(&s->sig_lock);
    zstr_webrtc_bitrate_cb cb = s->bitrate_cb;
    void *ud = s->bitrate_ud;
    pthread_mutex_unlock(&s->sig_lock);
    if (cb) cb((uint64_t)bitrate, ud);
}

/* --- TWCC interceptor shims --- */
static void *twcc_incoming_shim(int pc, const char *msg, int size, void *ptr)
{
    (void)pc;
    zstr_webrtc_t *s = ptr;
    if (s && msg && size == 12) {
        const uint8_t *b = (const uint8_t *)msg;
        /* RTCP PSFB/PLI is exactly 12 bytes; our dynamic RTP PTs
         * (96/111/126) can never alias PT byte 206. */
        if (b[1] == 206 && (b[0] & 0x1F) == 1)
            s->pli_rx_count++;
    }
    if (s && s->twcc) return zstr_webrtc_twcc_process_incoming(s->twcc, msg, size);
    return (void *)msg;
}

static void *twcc_outgoing_shim(int tr, const char *msg, int size, void *ptr)
{
    (void)tr;
    zstr_webrtc_t *s = ptr;
    if (s && s->twcc) return zstr_webrtc_twcc_process_outgoing(s->twcc, msg, size);
    return (void *)msg;
}

/* --- libdatachannel callbacks --- */
static void on_local_description(int pc, const char *sdp, const char *type, void *ptr)
{
    (void)pc;
    zstr_webrtc_t *s = ptr;
    if (!s || !sdp || !type) return;
    /* Chrome compat: normalize our generated SDP (BUNDLE/rtcp-mux/msid). */
    char *compat = zstr_sdp_compat_local(sdp);
    const char *store = compat ? compat : sdp;
    /* TWCC extmap injection for answers (documents the negotiated extmap;
     * the SDP is already committed inside libdatachannel). */
    char *with_twcc = NULL;
    if (s->twcc && strcmp(type, "answer") == 0) {
        size_t cap = strlen(store) + 2048;
        with_twcc = malloc(cap);
        if (with_twcc) {
            snprintf(with_twcc, cap, "%s", store);
            if (zstr_webrtc_twcc_inject_answer(s->twcc, with_twcc, cap) == 0)
                store = with_twcc;
            else {
                free(with_twcc);
                with_twcc = NULL;
            }
        }
    }
    pthread_mutex_lock(&s->sdp_lock);
    s->sdp_slot ^= 1;
    snprintf(s->local_sdp[s->sdp_slot], sizeof(s->local_sdp[0]), "%s", store);
    snprintf(s->local_type, sizeof(s->local_type), "%s", type);
    s->sdp_gen++;
    pthread_cond_signal(&s->sdp_cond);
    pthread_mutex_unlock(&s->sdp_lock);
    free(compat);
    free(with_twcc);
}

static void on_local_candidate(int pc, const char *cand, const char *mid, void *ptr)
{
    (void)pc;
    zstr_webrtc_t *s = ptr;
    if (!s || !cand) return;
    if (cand[0] == '\0') return; /* end-of-candidates */
    zstr_webrtc_ice_cb cb;
    void *ud;
    pthread_mutex_lock(&s->sig_lock);
    cb = s->ice_cb;
    ud = s->ice_ud;
    pthread_mutex_unlock(&s->sig_lock);
    if (cb) cb(cand, mid ? mid : "", ud);
}

static void on_state_change(int id, rtcState state, void *ptr)
{
    (void)id;
    zstr_webrtc_t *s = ptr;
    if (!s) return;
    pthread_mutex_lock(&s->sig_lock);
    s->conn_state = state;
    pthread_mutex_unlock(&s->sig_lock);
}

static void on_gathering_state(int pc, rtcGatheringState state, void *ptr)
{
    (void)pc;
    zstr_webrtc_t *s = ptr;
    if (!s) return;
    pthread_mutex_lock(&s->sdp_lock);
    s->gathering_state = state;
    pthread_cond_signal(&s->sdp_cond);
    pthread_mutex_unlock(&s->sdp_lock);
}

static void rx_push(zstr_webrtc_t *s, AVPacket *pkt, int track_idx)
{
    pthread_mutex_lock(&s->rx_lock);
    if (s->rx_count >= ZSTR_WEBRTC_RX_CAP) {
        /* Drop oldest (live semantics) */
        av_packet_free(&s->rx[s->rx_head].pkt);
        s->rx_head = (s->rx_head + 1) % ZSTR_WEBRTC_RX_CAP;
        s->rx_count--;
    }
    s->rx[s->rx_tail].pkt = pkt;
    s->rx[s->rx_tail].track_idx = track_idx;
    s->rx_tail = (s->rx_tail + 1) % ZSTR_WEBRTC_RX_CAP;
    s->rx_count++;
    pthread_cond_signal(&s->rx_cond);
    pthread_mutex_unlock(&s->rx_lock);
}

static void on_frame(int tr, const char *data, int size, const rtcFrameInfo *info, void *ptr)
{
    zstr_webrtc_t *s = ptr;
    if (!s || !data || size <= 0) return;

    /* Resolve codec clock + track index from the recv table */
    uint32_t clock = 90000;
    int track_idx = 0;
    pthread_mutex_lock(&s->sig_lock);
    for (int i = 0; i < s->nb_recv; i++) {
        if (s->recv[i].active && s->recv[i].track_id == tr) {
            clock = s->recv[i].clock_rate;
            track_idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&s->sig_lock);

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return;
    if (av_new_packet(pkt, size) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, data, size);

    /* PTS from RTP timestamp when available */
    if (info && info->timestampSeconds >= 0) {
        pkt->pts = (int64_t)(info->timestampSeconds * 1000000000.0);
    } else if (info && info->timestamp > 0 && clock > 0) {
        pkt->pts = (int64_t)((uint64_t)info->timestamp * 1000000000ULL / clock);
    } else {
        pkt->pts = AV_NOPTS_VALUE;
    }
    pkt->dts = pkt->pts;
    pkt->stream_index = track_idx;

    rx_push(s, pkt, track_idx);
}

static void on_track(int pc, int tr, void *ptr)
{
    (void)pc;
    zstr_webrtc_t *s = ptr;
    if (!s) return;
    char sdp_buf[1024] = { 0 };
    rtcGetTrackDescription(tr, sdp_buf, sizeof(sdp_buf));
    zstr_webrtc_codec_t codec = codec_from_sdp(sdp_buf);

    pthread_mutex_lock(&s->sig_lock);
    if (s->nb_recv < ZSTR_WEBRTC_MAX_TRACKS) {
        s->recv[s->nb_recv].track_id = tr;
        s->recv[s->nb_recv].clock_rate = codec_clock_rate(codec);
        s->recv[s->nb_recv].active = true;
        s->nb_recv++;
    }
    pthread_mutex_unlock(&s->sig_lock);


    /* Attach the matching depacketizer so on_frame receives full frames.
     * (Sending tracks need explicit packetizers; remote tracks need the
     * mirror depacketizer — libdatachannel does not auto-attach it.) */
    switch (codec) {
        case ZSTR_WEBRTC_CODEC_H264:
            rtcSetH264Depacketizer(tr, RTC_NAL_SEPARATOR_START_SEQUENCE);
            break;
        case ZSTR_WEBRTC_CODEC_H265:
            rtcSetH265Depacketizer(tr, RTC_NAL_SEPARATOR_START_SEQUENCE);
            break;
        case ZSTR_WEBRTC_CODEC_VP8:  rtcSetVP8Depacketizer(tr); break;
        case ZSTR_WEBRTC_CODEC_VP9:  rtcSetVP9Depacketizer(tr); break;
        case ZSTR_WEBRTC_CODEC_OPUS: rtcSetOpusDepacketizer(tr); break;
        case ZSTR_WEBRTC_CODEC_AAC:  rtcSetAACDepacketizer(tr); break;
        case ZSTR_WEBRTC_CODEC_PCMU: rtcSetPCMUDepacketizer(tr); break;
        case ZSTR_WEBRTC_CODEC_PCMA: rtcSetPCMADepacketizer(tr); break;
        default: break;
    }

    /* RTCP receiving session: learns sender SSRCs from inbound RTP so that
     * rtcRequestKeyframe() on this track emits a routable PLI. Without it
     * the request targets SSRC 0 and dies in the sender's SSRC demux. */
    rtcChainRtcpReceivingSession(tr);

    /* PliHandler on recv tracks too: RTCP demux may deliver a PLI/FIR to
     * any track object carrying a matching SSRC association, and only
     * tracks with a PliHandler surface it to keyframe_cb. Harmless for
     * media (pass-through for non-feedback RTCP). */
    rtcChainPliHandler(tr, on_pli_shim);

    /* Frame callback routes into the shared FIFO */
    rtcSetUserPointer(tr, s);
    rtcSetFrameCallback(tr, on_frame);
    if (s->twcc_enable)
        rtcSetTrackInterceptorCallback(tr, twcc_outgoing_shim);
}

static void on_dc_open(int id, void *ptr);
static void on_dc_message(int id, const char *message, int size, void *ptr);

static void on_data_channel(int pc, int dc, void *ptr)
{
    (void)pc;
    zstr_webrtc_t *s = ptr;
    if (!s) return;
    char label[64] = { 0 };
    rtcGetDataChannelLabel(dc, label, sizeof(label) - 1);
    pthread_mutex_lock(&s->sig_lock);
    int slot = -1;
    if (s->nb_dcs < ZSTR_WEBRTC_MAX_DC) {
        slot = s->nb_dcs++;
        snprintf(s->dcs[slot].label, sizeof(s->dcs[slot].label), "%s", label);
        s->dcs[slot].dc_id = dc;
        s->dcs[slot].open = true;
    }
    pthread_mutex_unlock(&s->sig_lock);
    (void)slot;
    /* Remote-created channels need handlers too (creator side sets its own). */
    rtcSetUserPointer(dc, s);
    rtcSetOpenCallback(dc, on_dc_open);
    rtcSetMessageCallback(dc, on_dc_message);
}

static void on_dc_open(int id, void *ptr)
{
    zstr_webrtc_t *s = ptr;
    if (!s) return;
    /* Match by id first; fall back to label for the create race where
     * open fires before create_data_channel stores dc_id. */
    char label[64] = "";
    rtcGetDataChannelLabel(id, label, sizeof(label) - 1);
    pthread_mutex_lock(&s->sig_lock);
    for (int i = 0; i < s->nb_dcs; i++) {
        if (s->dcs[i].dc_id == id ||
            (s->dcs[i].dc_id == -1 && label[0] &&
             strcmp(s->dcs[i].label, label) == 0)) {
            s->dcs[i].dc_id = id;
            s->dcs[i].open = true;
        }
    }
    pthread_mutex_unlock(&s->sig_lock);
}

static void on_dc_message(int id, const char *message, int size, void *ptr)
{
    zstr_webrtc_t *s = ptr;
    if (!s || !message || size <= 0) return;
    char label[64] = "";
    pthread_mutex_lock(&s->sig_lock);
    for (int i = 0; i < s->nb_dcs; i++) {
        if (s->dcs[i].dc_id == id) {
            snprintf(label, sizeof(label), "%s", s->dcs[i].label);
            break;
        }
    }
    zstr_webrtc_dc_message_cb cb = s->dc_cb;
    void *ud = s->dc_ud;
    pthread_mutex_unlock(&s->sig_lock);
    /* Binary vs string: libdatachannel uses negative size for strings */
    if (cb) {
        if (size < 0) cb(label, (const uint8_t *)message, (size_t)(-size), false, ud);
        else cb(label, (const uint8_t *)message, (size_t)size, true, ud);
    }
}

/* --- Lifecycle --- */
zstr_webrtc_t *zstr_webrtc_alloc(const char *opt_string)
{
    zstr_webrtc_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->pc_id = -1;
    s->conn_state = RTC_NEW;
    s->gathering_state = RTC_GATHERING_NEW;
    s->twcc_enable = true;
    s->trickle = true;
    pthread_mutex_init(&s->sdp_lock, NULL);
    pthread_cond_init(&s->sdp_cond, NULL);
    pthread_mutex_init(&s->sig_lock, NULL);
    pthread_mutex_init(&s->rx_lock, NULL);
    pthread_cond_init(&s->rx_cond, NULL);
    for (int i = 0; i < ZSTR_WEBRTC_MAX_TRACKS; i++) s->tracks[i].track_id = -1;

    /* Parse "stun=host:port:turn=user:pass@host:port:twcc=0" */
    if (opt_string && opt_string[0]) {
        char *copy = strdup(opt_string);
        if (copy) {
            char *tok = strtok(copy, ":,;");
            while (tok) {
                char *eq = strchr(tok, '=');
                if (eq) {
                    *eq = '\0';
                    if (strcmp(tok, "stun") == 0 && s->nb_ice_servers < ZSTR_WEBRTC_MAX_ICE) {
                        char url[256];
                        snprintf(url, sizeof(url), "stun:%s", eq + 1);
                        s->ice_servers[s->nb_ice_servers++] = strdup(url);
                    } else if (strcmp(tok, "turn") == 0 && s->nb_ice_servers < ZSTR_WEBRTC_MAX_ICE) {
                        char url[256];
                        snprintf(url, sizeof(url), "turn:%s", eq + 1);
                        s->ice_servers[s->nb_ice_servers++] = strdup(url);
                    } else if (strcmp(tok, "twcc") == 0) {
                        s->twcc_enable = (atoi(eq + 1) != 0);
                    } else if (strcmp(tok, "trickle") == 0) {
                        s->trickle = (atoi(eq + 1) != 0);
                    } else if (strcmp(tok, "codec_pref") == 0 ||
                               strcmp(tok, "codec-preference") == 0) {
                        free(s->codec_pref);
                        s->codec_pref = strdup(eq + 1);
                    }
                }
                tok = strtok(NULL, ":,;");
            }
            free(copy);
        }
    }

    rtcConfiguration config = { 0 };
    config.iceServers = (const char **)s->ice_servers;
    config.iceServersCount = s->nb_ice_servers;
    /* Manual negotiation only: implicit offers race explicit setLocal and
     * can only be observed (never reliably consumed). */
    config.disableAutoNegotiation = 1;

    s->pc_id = rtcCreatePeerConnection(&config);
    if (s->pc_id < 0) {
        zstr_webrtc_free(&s);
        return NULL;
    }
    s->pc_created = true;
    rtcSetUserPointer(s->pc_id, s);
    rtcSetLocalDescriptionCallback(s->pc_id, on_local_description);
    rtcSetLocalCandidateCallback(s->pc_id, on_local_candidate);
    rtcSetStateChangeCallback(s->pc_id, on_state_change);
    rtcSetGatheringStateChangeCallback(s->pc_id, on_gathering_state);
    rtcSetTrackCallback(s->pc_id, on_track);
    rtcSetDataChannelCallback(s->pc_id, on_data_channel);

    if (s->twcc_enable) {
        s->twcc = zstr_webrtc_twcc_create(NULL, NULL);
        rtcSetMediaInterceptorCallback(s->pc_id, twcc_incoming_shim);
    }
    return s;
}

void zstr_webrtc_free(zstr_webrtc_t **ps)
{
    if (!ps || !*ps) return;
    zstr_webrtc_t *s = *ps;

    if (s->twcc) zstr_webrtc_twcc_destroy(s->twcc);
    if (s->pc_created && s->pc_id >= 0) {
        rtcClosePeerConnection(s->pc_id);
        rtcDeletePeerConnection(s->pc_id);
    }
    for (int i = 0; i < s->nb_ice_servers; i++) free(s->ice_servers[i]);
    free(s->codec_pref);

    pthread_mutex_lock(&s->rx_lock);
    while (s->rx_count > 0) {
        av_packet_free(&s->rx[s->rx_head].pkt);
        s->rx_head = (s->rx_head + 1) % ZSTR_WEBRTC_RX_CAP;
        s->rx_count--;
    }
    pthread_mutex_unlock(&s->rx_lock);

    pthread_mutex_destroy(&s->sdp_lock);
    pthread_cond_destroy(&s->sdp_cond);
    pthread_mutex_destroy(&s->sig_lock);
    pthread_mutex_destroy(&s->rx_lock);
    pthread_cond_destroy(&s->rx_cond);
    free(s);
    *ps = NULL;
}

void zstr_webrtc_set_bitrate_cb(zstr_webrtc_t *s, zstr_webrtc_bitrate_cb cb, void *ud)
{
    if (!s) return;
    /* Re-create TWCC session with the app callback */
    if (s->twcc) zstr_webrtc_twcc_destroy(s->twcc);
    s->twcc = s->twcc_enable ? zstr_webrtc_twcc_create(cb, ud) : NULL;
}

void zstr_webrtc_set_ice_cb(zstr_webrtc_t *s, zstr_webrtc_ice_cb cb, void *ud)
{
    if (!s) return;
    pthread_mutex_lock(&s->sig_lock);
    s->ice_cb = cb;
    s->ice_ud = ud;
    pthread_mutex_unlock(&s->sig_lock);
}

void zstr_webrtc_set_dc_message_cb(zstr_webrtc_t *s, zstr_webrtc_dc_message_cb cb, void *ud)
{
    if (!s) return;
    pthread_mutex_lock(&s->sig_lock);
    s->dc_cb = cb;
    s->dc_ud = ud;
    pthread_mutex_unlock(&s->sig_lock);
}

/* --- Tracks --- */
static int add_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                     uint8_t pt, uint32_t clock, bool audio, bool recvonly,
                     const char *force_mid)
{
    if (!s || !s->pc_created || s->nb_tracks >= ZSTR_WEBRTC_MAX_TRACKS) return -1;
    webrtc_track_t *t = &s->tracks[s->nb_tracks];
    t->codec = codec;
    t->clock_rate = clock ? clock : codec_clock_rate(codec);
    t->payload_type = pt;
    t->is_audio = audio;
    /* MID must be unique per engine: the answerer associates remote tracks
     * by MID, and a collision with its own local tracks suppresses on_track.
     * Exception: answer tracks deliberately reuse the OFFERED mid so the
     * local send transceiver binds to the offered m-line (standard answer
     * semantics); only valid when the remote side sends nothing on that
     * MID (i.e. it offered recvonly/inactive). */
    if (force_mid && force_mid[0])
        snprintf(t->mid, sizeof(t->mid), "%s", force_mid);
    else
        snprintf(t->mid, sizeof(t->mid), "%s%d-%d",
                 audio ? "audio" : "video", s->nb_tracks, s->pc_id);

    rtcTrackInit tinit = { 0 };
    tinit.direction = recvonly ? RTC_DIRECTION_RECVONLY : RTC_DIRECTION_SENDONLY;
    tinit.codec = codec_to_rtc(codec);
    tinit.payloadType = (int)pt;
    tinit.ssrc = (uint32_t)rand() ^ (uint32_t)(uintptr_t)s;
    tinit.mid = t->mid;
    tinit.name = "zff";
    tinit.msid = "stream0";
    char tid[64];
    snprintf(tid, sizeof(tid), "track-%s", t->mid);
    tinit.trackId = tid;

    int tr = rtcAddTrackEx(s->pc_id, &tinit);
    if (tr < 0) return -1;
    t->track_id = tr;
    t->ssrc = tinit.ssrc;

    if (recvonly) {
        /* Receiver-side RTCP only: NACK responder + REMB for congestion
         * feedback. No packetizer, no sender reports, no outgoing TWCC. */
        rtcChainRtcpNackResponder(tr, RTC_DEFAULT_MAXIMUM_PACKET_COUNT_FOR_NACK_CACHE);
        rtcChainRembHandler(tr, on_remb_shim);
        /* Local recvonly transceivers are first-class receivers: attach
         * the depacketizer + frame callback and register in the recv
         * table. Inbound RTP is routed to the local transceiver object
         * (not the on_track handle) when their MIDs coincide; without
         * this, WHEP-style players connect but never receive.
         * Single delivery is guaranteed: exactly one of the two paths
         * carries frames per topology (verified by test drain). */
        switch (codec) {
            case ZSTR_WEBRTC_CODEC_H264:
                rtcSetH264Depacketizer(tr, RTC_NAL_SEPARATOR_START_SEQUENCE);
                break;
            case ZSTR_WEBRTC_CODEC_H265:
                rtcSetH265Depacketizer(tr, RTC_NAL_SEPARATOR_START_SEQUENCE);
                break;
            case ZSTR_WEBRTC_CODEC_VP8:  rtcSetVP8Depacketizer(tr); break;
            case ZSTR_WEBRTC_CODEC_VP9:  rtcSetVP9Depacketizer(tr); break;
            case ZSTR_WEBRTC_CODEC_OPUS: rtcSetOpusDepacketizer(tr); break;
            default: break;
        }
        rtcSetUserPointer(tr, s);
        rtcSetFrameCallback(tr, on_frame);
        pthread_mutex_lock(&s->sig_lock);
        if (s->nb_recv < ZSTR_WEBRTC_MAX_TRACKS) {
            s->recv[s->nb_recv].track_id = tr;
            s->recv[s->nb_recv].clock_rate = t->clock_rate;
            s->recv[s->nb_recv].active = true;
            s->nb_recv++;
        }
        pthread_mutex_unlock(&s->sig_lock);
        return s->nb_tracks++;
    }

    /* Attach the codec packetizer (required: sending without one throws).
     * Mirrors zstreamer: H264/H265 expect Annex-B start codes. */
    rtcPacketizerInit pinit = { 0 };
    pinit.ssrc = t->ssrc;
    pinit.cname = "zff";
    pinit.payloadType = (int)pt;
    pinit.clockRate = t->clock_rate;
    pinit.sequenceNumber = 0;
    pinit.timestamp = 0;
    if (audio) {
        switch (codec) {
            case ZSTR_WEBRTC_CODEC_AAC:  rtcSetAACPacketizer(tr, &pinit); break;
            case ZSTR_WEBRTC_CODEC_PCMU: rtcSetPCMUPacketizer(tr, &pinit); break;
            case ZSTR_WEBRTC_CODEC_PCMA: rtcSetPCMAPacketizer(tr, &pinit); break;
            default:                     rtcSetOpusPacketizer(tr, &pinit); break;
        }
    } else {
        switch (codec) {
            case ZSTR_WEBRTC_CODEC_VP8:  rtcSetVP8Packetizer(tr, &pinit); break;
            case ZSTR_WEBRTC_CODEC_VP9:  rtcSetVP9Packetizer(tr, &pinit); break;
            case ZSTR_WEBRTC_CODEC_H265:
                pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
                rtcSetH265Packetizer(tr, &pinit);
                break;
            default: /* H264 */
                pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
                rtcSetH264Packetizer(tr, &pinit);
                break;
        }
    }

    rtcChainPliHandler(tr, on_pli_shim);
    if (s->twcc_enable)
        rtcSetTrackInterceptorCallback(tr, twcc_outgoing_shim);
    /* RTCP QoS chain: ReceivingSession routes incoming RTCP (PLI included)
     * to the handlers; SrReporter emits sender reports for receivers. */
    rtcChainRtcpReceivingSession(tr);
    rtcChainRtcpSrReporter(tr);
    rtcChainRtcpNackResponder(tr, RTC_DEFAULT_MAXIMUM_PACKET_COUNT_FOR_NACK_CACHE);
    rtcChainRembHandler(tr, on_remb_shim);
    return s->nb_tracks++;
}

int zstr_webrtc_add_video_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                                uint8_t pt, uint32_t clock)
{
    return add_track(s, codec, pt, clock, false, false, NULL);
}

int zstr_webrtc_add_audio_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                                uint8_t pt, uint32_t clock)
{
    return add_track(s, codec, pt, clock, true, false, NULL);
}

/* Recvonly transceivers for WHEP-style playback (no local sender). */
int zstr_webrtc_add_recv_video_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                                     uint8_t pt, uint32_t clock)
{
    return add_track(s, codec, pt, clock, false, true, NULL);
}

int zstr_webrtc_add_recv_audio_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                                     uint8_t pt, uint32_t clock)
{
    return add_track(s, codec, pt, clock, true, true, NULL);
}

/* Answer-side send tracks: bind to the offered m-line by reusing its MID.
 * Call after set_remote_description(offer), before create_answer. Only
 * valid when the offerer sends nothing on that section (recvonly). */
int zstr_webrtc_add_answer_video_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                                       uint8_t pt, uint32_t clock)
{
    if (!s || !s->offered_video_mid[0]) return -1;
    return add_track(s, codec, pt, clock, false, false, s->offered_video_mid);
}

int zstr_webrtc_add_answer_audio_track(zstr_webrtc_t *s, zstr_webrtc_codec_t codec,
                                       uint8_t pt, uint32_t clock)
{
    if (!s || !s->offered_audio_mid[0]) return -1;
    return add_track(s, codec, pt, clock, true, false, s->offered_audio_mid);
}

/* --- Signaling --- */
/* Wait until sdp_gen advances past baseline (or timeout). */
static const char *wait_sdp_gen(zstr_webrtc_t *s, uint64_t baseline, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->sdp_lock);
    while (s->sdp_gen <= baseline) {
        if (pthread_cond_timedwait(&s->sdp_cond, &s->sdp_lock, &ts) == ETIMEDOUT) break;
    }
    const char *ret = (s->sdp_gen > baseline) ? s->local_sdp[s->sdp_slot] : NULL;
    pthread_mutex_unlock(&s->sdp_lock);
    return ret;
}

/* Wait until ICE gathering completes (or timeout). Returns 0 on complete. */
int zstr_webrtc_wait_gathering(zstr_webrtc_t *s, int timeout_ms)
{
    if (!s) return -1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->sdp_lock);
    while (s->gathering_state != RTC_GATHERING_COMPLETE) {
        if (pthread_cond_timedwait(&s->sdp_cond, &s->sdp_lock, &ts) == ETIMEDOUT) break;
    }
    int ok = (s->gathering_state == RTC_GATHERING_COMPLETE) ? 0 : -1;
    pthread_mutex_unlock(&s->sdp_lock);
    return ok;
}

/* For non-trickle exchange: wait for gathering, then re-read the committed
 * local description (now with candidates embedded) into the active slot. */
const char *zstr_webrtc_complete_gathering(zstr_webrtc_t *s, int timeout_ms)
{
    if (!s || !s->pc_created) return NULL;
    zstr_webrtc_wait_gathering(s, timeout_ms); /* best effort; backstop is connect timeout */
    pthread_mutex_lock(&s->sdp_lock);
    s->sdp_slot ^= 1;
    char *dst = s->local_sdp[s->sdp_slot];
    pthread_mutex_unlock(&s->sdp_lock);
    if (rtcGetLocalDescription(s->pc_id, dst, ZSTR_WEBRTC_SDP_CAP) < 0) return NULL;
    pthread_mutex_lock(&s->sdp_lock);
    s->sdp_gen++;
    pthread_cond_signal(&s->sdp_cond);
    pthread_mutex_unlock(&s->sdp_lock);
    return dst;
}

const char *zstr_webrtc_create_offer(zstr_webrtc_t *s)
{
    if (!s || !s->pc_created) return NULL;
    pthread_mutex_lock(&s->sdp_lock);
    uint64_t baseline = s->sdp_gen;
    pthread_mutex_unlock(&s->sdp_lock);
    if (rtcSetLocalDescription(s->pc_id, "offer") != RTC_ERR_SUCCESS) return NULL;
    /* TWCC extmap is negotiated from the remote offer; nothing to inject here */
    const char *sdp = wait_sdp_gen(s, baseline, 5000);
    if (sdp && !s->trickle)
        sdp = zstr_webrtc_complete_gathering(s, 5000);
    return sdp;
}

const char *zstr_webrtc_create_answer(zstr_webrtc_t *s)
{
    if (!s || !s->pc_created) return NULL;
    /* Manual negotiation: the answer is generated explicitly here (the
     * implicit path is disabled along with auto-negotiation). */
    pthread_mutex_lock(&s->sdp_lock);
    uint64_t baseline = s->sdp_gen;
    pthread_mutex_unlock(&s->sdp_lock);
    if (rtcSetLocalDescription(s->pc_id, "answer") != RTC_ERR_SUCCESS) return NULL;
    const char *sdp = wait_sdp_gen(s, baseline, 5000);
    if (sdp && !s->trickle)
        sdp = zstr_webrtc_complete_gathering(s, 5000);
    if (sdp) {
        /* TWCC extmap injection documents the extmap for the app; the SDP
         * is already committed inside libdatachannel. */
        if (s->twcc) {
            char buf[ZSTR_WEBRTC_SDP_CAP];
            snprintf(buf, sizeof(buf), "%s", sdp);
            zstr_webrtc_twcc_inject_answer(s->twcc, buf, sizeof(buf));
        }
    }
    return sdp;
}

/* Extract first video/first audio section MIDs from a remote offer.
 * Used by answer tracks: reusing the offered MID binds the local send
 * transceiver to the offered m-line (standard WebRTC answer semantics).
 * Safe only when the remote side sends nothing on that MID (recvonly). */
static void extract_offered_mids(zstr_webrtc_t *s, const char *sdp)
{
    s->offered_video_mid[0] = '\0';
    s->offered_audio_mid[0] = '\0';
    if (!sdp) return;
    int cur_media = 0; /* 1 = video, 2 = audio */
    const char *p = sdp;
    const char *end = sdp + strlen(sdp);
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        size_t len = (size_t)(eol - p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n')) len--;
        if (len >= 7 && strncmp(p, "m=video", 7) == 0) cur_media = 1;
        else if (len >= 7 && strncmp(p, "m=audio", 7) == 0) cur_media = 2;
        else if (len > 6 && strncmp(p, "a=mid:", 6) == 0 && cur_media) {
            size_t mlen = len - 6;
            if (cur_media == 1 && !s->offered_video_mid[0]) {
                if (mlen >= sizeof(s->offered_video_mid))
                    mlen = sizeof(s->offered_video_mid) - 1;
                memcpy(s->offered_video_mid, p + 6, mlen);
                s->offered_video_mid[mlen] = '\0';
            } else if (cur_media == 2 && !s->offered_audio_mid[0]) {
                if (mlen >= sizeof(s->offered_audio_mid))
                    mlen = sizeof(s->offered_audio_mid) - 1;
                memcpy(s->offered_audio_mid, p + 6, mlen);
                s->offered_audio_mid[mlen] = '\0';
            }
        }
        p = (eol < end) ? eol + 1 : end;
    }
}

int zstr_webrtc_set_remote_description(zstr_webrtc_t *s, const char *sdp, const char *type)
{
    if (!s || !s->pc_created || !sdp || !type) return -1;
    /* TWCC parses the RAW offer (transport-cc lines are kept by our filter,
     * but parse first so nothing can strip them). */
    if (s->twcc && strcmp(type, "offer") == 0)
        zstr_webrtc_twcc_parse_offer(s->twcc, sdp);

    char *filtered = zstr_sdp_filter(sdp);
    const char *stage = filtered ? filtered : sdp;
    char *selected = NULL;
    if (strcmp(type, "offer") == 0) {
        extract_offered_mids(s, sdp); /* raw offer: mid lines unfiltered */
        selected = zstr_sdp_select_codecs(stage, s->codec_pref,
                                          s->selected_video, sizeof(s->selected_video),
                                          s->selected_audio, sizeof(s->selected_audio));
        if (selected) stage = selected;
    }

    /* NOTE: no engine lock across rtc calls — libdatachannel may dispatch
     * on_track/on_data_channel synchronously, and those take sig_lock. */
    int ret = rtcSetRemoteDescription(s->pc_id, stage, type);
    free(filtered);
    free(selected);
    return ret == RTC_ERR_SUCCESS ? 0 : -1;
}

int zstr_webrtc_selected_codecs(const zstr_webrtc_t *s,
                                char *video_out, size_t video_len,
                                char *audio_out, size_t audio_len)
{
    if (!s) return -1;
    if (video_out && video_len > 0)
        snprintf(video_out, video_len, "%s", s->selected_video);
    if (audio_out && audio_len > 0)
        snprintf(audio_out, audio_len, "%s", s->selected_audio);
    return 0;
}

int zstr_webrtc_add_ice_candidate(zstr_webrtc_t *s, const char *cand, const char *mid)
{
    if (!s || !s->pc_created || !cand) return -1;
    int ret = rtcAddRemoteCandidate(s->pc_id, cand, mid ? mid : "");
    return ret == RTC_ERR_SUCCESS ? 0 : -1;
}

int zstr_webrtc_connected(zstr_webrtc_t *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->sig_lock);
    rtcState st = s->conn_state;
    pthread_mutex_unlock(&s->sig_lock);
    return st == RTC_CONNECTED ? 1 : (st == RTC_FAILED || st == RTC_CLOSED || st == RTC_DISCONNECTED ? 2 : 0);
}

int zstr_webrtc_wait_connected(zstr_webrtc_t *s, int timeout_ms)
{
    int64_t deadline = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;
    for (;;) {
        int st = zstr_webrtc_connected(s);
        if (st == 1) return 0;
        if (st == 2) return -1;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if ((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 >= deadline) return -1;
        struct timespec sleep = { 0, 20 * 1000 * 1000 };
        nanosleep(&sleep, NULL);
    }
}

void zstr_webrtc_set_keyframe_cb(zstr_webrtc_t *s, zstr_webrtc_keyframe_cb cb,
                                 void *user_data)
{
    if (!s) return;
    pthread_mutex_lock(&s->sig_lock);
    s->keyframe_cb = cb;
    s->keyframe_ud = user_data;
    pthread_mutex_unlock(&s->sig_lock);
}

int zstr_webrtc_request_keyframe(zstr_webrtc_t *s, int track_idx)
{
    if (!s || track_idx < 0) return -1;
    /* Prefer the RECV track: it owns a transport once media flows, so the
     * session's PLI actually emits. A SEND track that never transmitted has
     * no bound transport and the request throws inside libdatachannel.
     * Fall back to send tracks (sender-only topologies). */
    int tr = -1;
    pthread_mutex_lock(&s->sig_lock);
    for (int i = 0; i < s->nb_recv && tr < 0; i++) {
        if (s->recv[i].active && i == track_idx)
            tr = s->recv[i].track_id;
    }
    if (tr < 0 && track_idx < s->nb_tracks &&
        s->tracks[track_idx].track_id >= 0)
        tr = s->tracks[track_idx].track_id;
    pthread_mutex_unlock(&s->sig_lock);
    if (tr < 0) return -1;
    return rtcRequestKeyframe(tr) == RTC_ERR_SUCCESS ? 0 : -1;
}

/* --- Media --- */
int zstr_webrtc_send_media(zstr_webrtc_t *s, int track_idx, const AVPacket *pkt)
{
    if (!s || !pkt || !pkt->data || pkt->size <= 0) return -1;
    if (track_idx < 0 || track_idx >= s->nb_tracks) return -1;
    webrtc_track_t *t = &s->tracks[track_idx];
    if (t->track_id < 0) return -1;

    if (pkt->pts != AV_NOPTS_VALUE && pkt->time_base.den > 0) {
        uint32_t rtp_ts = (uint32_t)av_rescale_q(pkt->pts, pkt->time_base,
                                                 (AVRational){ 1, (int)t->clock_rate });
        rtcSetTrackRtpTimestamp(t->track_id, rtp_ts);
    } else if (pkt->pts > 0) {
        /* Legacy ns convention (mirrors zstreamer pad path) */
        uint32_t rtp_ts = (uint32_t)(pkt->pts * t->clock_rate / 1000000000ULL);
        rtcSetTrackRtpTimestamp(t->track_id, rtp_ts);
    }

    int ret = rtcSendMessage(t->track_id, (const char *)pkt->data, pkt->size);
    return ret == RTC_ERR_SUCCESS ? 0 : -1;
}

int zstr_webrtc_recv_media(zstr_webrtc_t *s, AVPacket *pkt, int *track_idx,
                           int timeout_ms, bool *got_frame)
{
    if (!s || !pkt || !got_frame) return -1;
    *got_frame = false;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&s->rx_lock);
    while (s->rx_count == 0) {
        if (pthread_cond_timedwait(&s->rx_cond, &s->rx_lock, &ts) == ETIMEDOUT) break;
    }
    if (s->rx_count == 0) {
        pthread_mutex_unlock(&s->rx_lock);
        return AVERROR(ETIMEDOUT);
    }
    AVPacket *q = s->rx[s->rx_head].pkt;
    int ti = s->rx[s->rx_head].track_idx;
    s->rx[s->rx_head].pkt = NULL;
    s->rx_head = (s->rx_head + 1) % ZSTR_WEBRTC_RX_CAP;
    s->rx_count--;
    pthread_mutex_unlock(&s->rx_lock);

    av_packet_move_ref(pkt, q);
    av_packet_free(&q);
    if (track_idx) *track_idx = ti;
    *got_frame = true;
    return 0;
}

/* --- Data channels --- */
int zstr_webrtc_create_data_channel(zstr_webrtc_t *s, const char *label)
{
    if (!s || !s->pc_created || !label) return -1;
    pthread_mutex_lock(&s->sig_lock);
    if (s->nb_dcs >= ZSTR_WEBRTC_MAX_DC) {
        pthread_mutex_unlock(&s->sig_lock);
        return -1;
    }
    int slot = s->nb_dcs++;
    snprintf(s->dcs[slot].label, sizeof(s->dcs[slot].label), "%s", label);
    s->dcs[slot].dc_id = -1;
    s->dcs[slot].open = false;
    pthread_mutex_unlock(&s->sig_lock);

    /* NOTE: rtc call outside the lock (see set_remote_description). */
    int dc = rtcCreateDataChannel(s->pc_id, label);
    if (dc < 0) {
        pthread_mutex_lock(&s->sig_lock);
        s->nb_dcs--;
        pthread_mutex_unlock(&s->sig_lock);
        return -1;
    }
    rtcSetUserPointer(dc, s);
    rtcSetOpenCallback(dc, on_dc_open);
    rtcSetMessageCallback(dc, on_dc_message);

    pthread_mutex_lock(&s->sig_lock);
    s->dcs[slot].dc_id = dc;
    pthread_mutex_unlock(&s->sig_lock);
    return 0;
}

int zstr_webrtc_send_data(zstr_webrtc_t *s, const char *label,
                          const uint8_t *data, size_t size)
{
    if (!s || !label || !data || size == 0) return -1;
    int dc = -1;
    pthread_mutex_lock(&s->sig_lock);
    for (int i = 0; i < s->nb_dcs; i++) {
        if (strcmp(s->dcs[i].label, label) == 0 && s->dcs[i].open) {
            dc = s->dcs[i].dc_id;
            break;
        }
    }
    pthread_mutex_unlock(&s->sig_lock);
    if (dc < 0) return -1;
    int ret = rtcSendMessage(dc, (const char *)data, (int)size);
    return ret == RTC_ERR_SUCCESS ? 0 : -1;
}

uint64_t zstr_webrtc_bitrate(const zstr_webrtc_t *s)
{
    if (!s || !s->twcc) return 0;
    return zstr_webrtc_twcc_bitrate(s->twcc);
}

/* Diagnostics: number of PLI feedback packets observed inbound.
 * NOTE: in loopback, a peer's session stamps OUR OWN SSRC as the PLI
 * sender, which this libdatachannel build drops as looped-back traffic
 * before any track sees it — so keyframe_cb fires only for genuine
 * remote senders (e.g. Chrome). The counter proves emission+transport. */
uint64_t zstr_webrtc_pli_received(const zstr_webrtc_t *s)
{
    return s ? s->pli_rx_count : 0;
}
