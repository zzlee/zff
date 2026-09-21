/*=============================================================================
    zstr_webrtc_device.c — FFmpeg muxer/demuxer for zstr_webrtc

    Non-trickle file-based signaling (v1): the offerer writes its SDP to
    offer_file and polls answer_file; the answerer polls offer_file, writes
    its answer, and both wait for connection. Candidates are embedded
    (trickle=0), so no Trickle/WHIP HTTP round-trips are needed. This covers
    loopback, container-to-container, and static peer setups; WHIP/websocket
    signaling is the follow-up transport for browser interop.

    Muxer (publisher): streams -> tracks in order (video H264/VP8/VP9,
      audio Opus v1) -> offer file -> answer file -> connected -> send.
    Demuxer (subscriber): offer file -> answer file -> connected ->
      streams parsed from the offer's m-sections -> recv.
 =============================================================================*/
#define _GNU_SOURCE

#include "zff/plugins/zstr_webrtc.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

/* --- File-based signaling helpers --- */
static int write_file(const char *path, const char *data)
{
    if (!path || !path[0] || !data) return AVERROR(EINVAL);
    FILE *f = fopen(path, "w");
    if (!f) return AVERROR(errno);
    size_t len = strlen(data);
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? 0 : AVERROR(EIO);
}

static char *read_file(const char *path)
{
    if (!path || !path[0]) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 65536) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t r = fread(buf, 1, len, f);
    fclose(f);
    buf[r] = '\0';
    return buf;
}

/* Poll until path exists with nonzero size, or timeout_ms expires. */
static int poll_file(const char *path, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) return 0;
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        waited += 100;
    }
    return AVERROR(ETIMEDOUT);
}

/* --- Minimal SDP m-section parser: counts media + first rtpmap each --- */
typedef struct {
    char media[16];   /* "video" / "audio" */
    char codec[32];   /* "H264" / "VP8" / "VP9" / "opus" ... */
    int payload_type;
    int clock_rate;
} sdp_media_t;

static int parse_offer_media(const char *sdp, sdp_media_t *out, int cap)
{
    int n = 0;
    const char *p = sdp;
    sdp_media_t cur;
    int have_cur = 0;
    memset(&cur, 0, sizeof(cur));

    char line[512];
    while (*p && n < cap) {
        size_t i = 0;
        while (*p && *p != '\n' && i + 1 < sizeof(line)) line[i++] = *p++;
        if (*p == '\n') p++;
        line[i] = '\0';
        size_t L = strlen(line);
        while (L > 0 && (line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = '\0';

        if (strncmp(line, "m=", 2) == 0) {
            if (have_cur) out[n++] = cur;
            memset(&cur, 0, sizeof(cur));
            have_cur = 0;
            char mtype[16] = "";
            if (sscanf(line + 2, "%15s", mtype) == 1 &&
                (strcmp(mtype, "video") == 0 || strcmp(mtype, "audio") == 0)) {
                snprintf(cur.media, sizeof(cur.media), "%s", mtype);
                have_cur = 1;
            }
        } else if (have_cur && cur.codec[0] == '\0' &&
                   strncmp(line, "a=rtpmap:", 9) == 0) {
            int pt = 0;
            char enc[32] = "";
            int clock = 0;
            if (sscanf(line + 9, "%d %31[^/]/%d", &pt, enc, &clock) >= 2) {
                cur.payload_type = pt;
                snprintf(cur.codec, sizeof(cur.codec), "%s", enc);
                cur.clock_rate = clock;
            }
        }
    }
    if (have_cur && n < cap) out[n++] = cur;
    return n;
}

static enum AVCodecID sdp_codec_to_id(const sdp_media_t *m)
{
    if (strcmp(m->media, "video") == 0) {
        if (strcasecmp(m->codec, "H264") == 0) return AV_CODEC_ID_H264;
        if (strcasecmp(m->codec, "VP8") == 0) return AV_CODEC_ID_VP8;
        if (strcasecmp(m->codec, "VP9") == 0) return AV_CODEC_ID_VP9;
        if (strcasecmp(m->codec, "H265") == 0) return AV_CODEC_ID_HEVC;
    } else {
        if (strcasecmp(m->codec, "opus") == 0) return AV_CODEC_ID_OPUS;
        if (strcasecmp(m->codec, "PCMU") == 0) return AV_CODEC_ID_PCM_MULAW;
        if (strcasecmp(m->codec, "PCMA") == 0) return AV_CODEC_ID_PCM_ALAW;
    }
    return AV_CODEC_ID_NONE;
}

/* --- Muxer (publisher) --- */
typedef struct WebRTCMuxContext {
    const AVClass *av_class;
    char *offer_file;
    char *answer_file;
    int handshake_timeout;
    zstr_webrtc_t *rtc;
} WebRTCMuxContext;

#define OFFSET_M(x) offsetof(WebRTCMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_webrtc_mux_options[] = {
    { "offer_file",  "Path to write the SDP offer",  OFFSET_M(offer_file),  AV_OPT_TYPE_STRING, { .str = "/tmp/zstr_webrtc_offer.sdp" }, 0, 0, ENC },
    { "answer_file", "Path to poll the SDP answer",  OFFSET_M(answer_file), AV_OPT_TYPE_STRING, { .str = "/tmp/zstr_webrtc_answer.sdp" }, 0, 0, ENC },
    { "handshake_timeout", "Signaling/connect timeout in ms", OFFSET_M(handshake_timeout), AV_OPT_TYPE_INT, { .i64 = 15000 }, 1000, 120000, ENC },
    { NULL }
};

static const AVClass zstr_webrtc_mux_class = {
    .class_name = "zstr_webrtc_mux",
    .item_name = av_default_item_name,
    .option = zstr_webrtc_mux_options,
    .version = LIBAVUTIL_VERSION_INT,
};

static int webrtc_mux_write_header(AVFormatContext *s)
{
    WebRTCMuxContext *ctx = s->priv_data;

    ctx->rtc = zstr_webrtc_alloc("twcc=1:trickle=0");
    if (!ctx->rtc) return AVERROR(ENOMEM);

    /* Streams -> tracks in order (track idx == stream idx) */
    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        AVCodecParameters *cp = st->codecpar;
        int idx = -1;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            zstr_webrtc_codec_t c;
            if (cp->codec_id == AV_CODEC_ID_H264) c = ZSTR_WEBRTC_CODEC_H264;
            else if (cp->codec_id == AV_CODEC_ID_VP8) c = ZSTR_WEBRTC_CODEC_VP8;
            else if (cp->codec_id == AV_CODEC_ID_VP9) c = ZSTR_WEBRTC_CODEC_VP9;
            else return AVERROR(EINVAL);
            idx = zstr_webrtc_add_video_track(ctx->rtc, c, 96, 90000);
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (cp->codec_id != AV_CODEC_ID_OPUS) return AVERROR(EINVAL); /* v1 Opus-only */
            idx = zstr_webrtc_add_audio_track(ctx->rtc, ZSTR_WEBRTC_CODEC_OPUS, 111, 48000);
        } else {
            return AVERROR(EINVAL);
        }
        if (idx != (int)i) return AVERROR(EINVAL); /* keep stream/track 1:1 */
    }
    if (s->nb_streams == 0) return AVERROR(EINVAL);

    const char *offer = zstr_webrtc_create_offer(ctx->rtc);
    if (!offer) return AVERROR(EIO);
    if (write_file(ctx->offer_file, offer) < 0) return AVERROR(EIO);

    if (poll_file(ctx->answer_file, ctx->handshake_timeout) < 0) return AVERROR(ETIMEDOUT);
    char *answer = read_file(ctx->answer_file);
    if (!answer) return AVERROR(EIO);
    int ret = zstr_webrtc_set_remote_description(ctx->rtc, answer, "answer");
    free(answer);
    if (ret < 0) return ret;

    return zstr_webrtc_wait_connected(ctx->rtc, ctx->handshake_timeout);
}

static int webrtc_mux_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    WebRTCMuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->rtc || !pkt) return 0;
    if (pkt->stream_index < 0 || pkt->stream_index >= (int)s->nb_streams) return 0;
    AVStream *st = s->streams[pkt->stream_index];
    AVPacket tmp = *pkt;
    if (tmp.time_base.den <= 0) tmp.time_base = st->time_base;
    return zstr_webrtc_send_media(ctx->rtc, pkt->stream_index, &tmp);
}

static int webrtc_mux_write_trailer(AVFormatContext *s)
{
    WebRTCMuxContext *ctx = s->priv_data;
    if (ctx && ctx->rtc) zstr_webrtc_free(&ctx->rtc);
    return 0;
}

const FFOutputFormat ff_zstr_webrtc_muxer = {
    .p = {
        .name = "zstr_webrtc",
        .long_name = "zff WebRTC Publisher (file-signaled)",
        .extensions = NULL,
        .audio_codec = AV_CODEC_ID_OPUS,
        .video_codec = AV_CODEC_ID_H264,
        .subtitle_codec = AV_CODEC_ID_NONE,
        .flags = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
        .priv_class = &zstr_webrtc_mux_class,
    },
    .priv_data_size = sizeof(WebRTCMuxContext),
    .write_header = webrtc_mux_write_header,
    .write_packet = webrtc_mux_write_packet,
    .write_trailer = webrtc_mux_write_trailer,
    .check_bitstream = NULL,
};

/* --- Demuxer (subscriber) --- */
typedef struct WebRTCDemuxContext {
    const AVClass *av_class;
    char *offer_file;
    char *answer_file;
    int handshake_timeout;
    zstr_webrtc_t *rtc;
} WebRTCDemuxContext;

#define OFFSET_D(x) offsetof(WebRTCDemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_webrtc_demux_options[] = {
    { "offer_file",  "Path to poll the SDP offer",  OFFSET_D(offer_file),  AV_OPT_TYPE_STRING, { .str = "/tmp/zstr_webrtc_offer.sdp" }, 0, 0, DEC },
    { "answer_file", "Path to write the SDP answer", OFFSET_D(answer_file), AV_OPT_TYPE_STRING, { .str = "/tmp/zstr_webrtc_answer.sdp" }, 0, 0, DEC },
    { "handshake_timeout", "Signaling/connect timeout in ms", OFFSET_D(handshake_timeout), AV_OPT_TYPE_INT, { .i64 = 15000 }, 1000, 120000, DEC },
    { NULL }
};

static const AVClass zstr_webrtc_demux_class = {
    .class_name = "zstr_webrtc_demux",
    .item_name = av_default_item_name,
    .option = zstr_webrtc_demux_options,
    .version = LIBAVUTIL_VERSION_INT,
};

static int webrtc_demux_read_header(AVFormatContext *s)
{
    WebRTCDemuxContext *ctx = s->priv_data;

    ctx->rtc = zstr_webrtc_alloc("twcc=1:trickle=0");
    if (!ctx->rtc) return AVERROR(ENOMEM);

    /* Answerer flow: read remote offer -> answer -> connected */
    if (poll_file(ctx->offer_file, ctx->handshake_timeout) < 0) return AVERROR(ETIMEDOUT);
    char *offer = read_file(ctx->offer_file);
    if (!offer) return AVERROR(EIO);

    /* Streams come from the offer's m-sections (pre-answer, pre-connect) */
    sdp_media_t media[8];
    int nb_media = parse_offer_media(offer, media, 8);
    if (nb_media <= 0) {
        free(offer);
        return AVERROR(EINVAL);
    }
    for (int i = 0; i < nb_media; i++) {
        enum AVCodecID cid = sdp_codec_to_id(&media[i]);
        if (cid == AV_CODEC_ID_NONE) {
            free(offer);
            return AVERROR(EINVAL);
        }
        AVStream *st = avformat_new_stream(s, NULL);
        if (!st) {
            free(offer);
            return AVERROR(ENOMEM);
        }
        st->codecpar->codec_type = (strcmp(media[i].media, "video") == 0)
                                   ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = cid;
        st->time_base = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                        ? (AVRational){ 1, 90000 } : (AVRational){ 1, 48000 };
        /* Payload type travels in-band via the engine (recv track order). */
        (void)media[i].payload_type;
    }

    if (zstr_webrtc_set_remote_description(ctx->rtc, offer, "offer") < 0) {
        free(offer);
        return AVERROR(EIO);
    }
    free(offer);

    const char *answer = zstr_webrtc_create_answer(ctx->rtc);
    if (!answer) return AVERROR(EIO);
    if (write_file(ctx->answer_file, answer) < 0) return AVERROR(EIO);

    return zstr_webrtc_wait_connected(ctx->rtc, ctx->handshake_timeout);
}

static int webrtc_demux_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WebRTCDemuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->rtc) return AVERROR(EINVAL);
    bool got = false;
    int track_idx = 0;
    /* Stream order follows offer m-section order; engine recv order matches
     * for the 1v+1a case (multi-track routing is a follow-up). */
    int ret = zstr_webrtc_recv_media(ctx->rtc, pkt, &track_idx, 5000, &got);
    if (ret < 0 || !got) return ret < 0 ? ret : AVERROR(EAGAIN);
    if (track_idx < 0 || track_idx >= (int)s->nb_streams) track_idx = 0;
    pkt->stream_index = track_idx;
    if (pkt->time_base.den <= 0) pkt->time_base = s->streams[track_idx]->time_base;
    return 0;
}

static int webrtc_demux_read_close(AVFormatContext *s)
{
    WebRTCDemuxContext *ctx = s->priv_data;
    if (ctx && ctx->rtc) zstr_webrtc_free(&ctx->rtc);
    return 0;
}

const AVInputFormat ff_zstr_webrtc_demuxer = {
    .name = "zstr_webrtc",
    .long_name = "zff WebRTC Subscriber (file-signaled)",
    .flags = AVFMT_NOFILE,
    .priv_class = &zstr_webrtc_demux_class,
    .priv_data_size = sizeof(WebRTCDemuxContext),
    .read_header = webrtc_demux_read_header,
    .read_packet = webrtc_demux_read_packet,
    .read_close = webrtc_demux_read_close,
};
