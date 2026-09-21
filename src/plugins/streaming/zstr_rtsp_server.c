/*=============================================================================
    zstr_rtsp_server.c — Multi-session RTSP Server (AVOutputFormat service)

    Ported from zstreamer `rtsp_server.c`. Keeps the protocol core, drops the
    zst_* framework (pads / scheduler / caps / timestamp pacer):

    Kept:
      - RTSP state machine (OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN)
      - SDP generation (H264 avcC sprop-parameter-sets, AAC MPEG4-GENERIC)
      - Transport negotiation (TCP interleaved + UDP unicast; multicast -> 461)
      - RTCP Sender Reports, TCP interleaved framing, Annex-B NAL splitting
      - Per-client threads + listener thread, SPS/PPS caching
    Replaced:
      - zst_buffer push callbacks  -> av_write_frame() fan-out by stream index
      - internal h264_packetize    -> zstr_rtp_payloader (FU-A / FU H265)
      - caps/extradata negotiation -> AVStream codecpar at write_header
      - UDP timestamp pacing       -> not in v1 (FFmpeg app paces delivery)
 =============================================================================*/
#define _GNU_SOURCE

#include "zff/plugins/zstr_rtsp_server.h"
#include "zff/plugins/zstr_rtp.h"
#include "zff/zff_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <poll.h>

#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/intreadwrite.h>

/* --- Constants (mirroring zstreamer) --- */
#define RTSP_BUF_SIZE      8192
#define RTSP_REPLY_SIZE    8192
#define RTSP_SDP_SIZE      4096
#define RTSP_SESSION_ID_LEN 32
#define RTCP_INTERVAL_MS   5000
#define MAX_CLIENTS        16

#define RTSP_TRANSPORT_TCP       0
#define RTSP_TRANSPORT_UDP       1
#define RTSP_TRANSPORT_MULTICAST 2

#define RTP_PT_H264   96
#define RTP_PT_H265   96
#define RTP_PT_AAC    97
#define RTP_CLOCK_VIDEO 90000

#define CODEC_H264 1
#define CODEC_H265 2
#define CODEC_AAC  3

#define H264_NAL_SPS 7
#define H264_NAL_PPS 8

/* --- Session: one mount point fed by write_packet --- */
typedef struct {
    char     name[64];
    int      has_video;
    int      has_audio;
    int      video_codec;   /* CODEC_H264 / CODEC_H265 */
    int      width;
    int      height;
    double   framerate;
    int      sample_rate;
    int      channels;
    uint8_t *extra_data;    /* avcC / AVCodec extradata copy for SDP */
    int      extra_size;
    uint8_t *sps_pps_cache; /* Annex-B SPS/PPS seen in-band */
    int      sps_pps_cache_size;
} rtsp_session_t;

/* --- Per-client, per-stream RTP state --- */
typedef struct {
    int                   codec;          /* 0 = not set up */
    uint32_t              ssrc;
    uint32_t              clock_rate;
    uint8_t               payload_type;
    int                   interleaved_ch; /* TCP channel for RTP (RTCP = +1) */
    zstr_rtp_payloader_t *payloader;      /* video only (AAC uses RFC3640 below) */
    int                   udp_rtp_fd;
    int                   udp_rtcp_fd;
    uint16_t              server_rtp_port;
    uint16_t              server_rtcp_port;
    struct sockaddr_in    client_rtp_addr;
    struct sockaddr_in    client_rtcp_addr;
    uint16_t              client_rtp_port;
    uint16_t              client_rtcp_port;
    int                   packet_count;
    int                   octet_count;
    uint32_t              last_rtp_ts;
    int                   sps_pps_sent;
} rtp_stream_t;

typedef struct rtsp_client_s {
    int      fd;
    char     peer_ip[64];
    uint16_t peer_port;
    char     buf[RTSP_BUF_SIZE];
    int      buf_len;
    char     method[16];
    char     uri[512];
    unsigned cseq;
    char     session_id[RTSP_SESSION_ID_LEN];
    char     transport_hdr[256];
    int      transport_type;
    int      interleaved_rtp;
    int      interleaved_rtcp;
    int      track_setup_mask;   /* bit0 = video, bit1 = audio */
    int      play_state;         /* 0=init, 1=playing, 2=paused */
    rtsp_session_t *session;     /* bound mount (single-session server) */
    rtp_stream_t vstream;
    rtp_stream_t astream;
    pthread_t thread;
    int      running;
    struct rtsp_server_s *server;
    struct rtsp_client_s *next;
} rtsp_client_t;

typedef struct rtsp_server_s {
    int            listen_fd;
    int            port;
    char           mount[64];
    int            force_tcp;
    int            mtu;
    pthread_mutex_t lock;
    rtsp_client_t *clients;
    int            client_count;
    rtsp_session_t session;
    int            running;
    pthread_t      listen_thread;
} rtsp_server_t;

/* --- Misc helpers --- */
static uint32_t rand32(void)
{
    uint32_t v = (uint32_t)rand();
    v ^= (uint32_t)(uintptr_t)&v >> 3;
    v ^= (uint32_t)time(NULL);
    return v ? v : 0x12345678;
}

static uint64_t ntp_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ntp = (uint64_t)(ts.tv_sec + 2208988800ULL) << 32;
    ntp |= (uint64_t)((double)ts.tv_nsec * 4.294967296);
    return ntp;
}

#define APPEND_SNPRINTF(buf, size, n, ...) do { \
    if ((n) < (size)) { \
        int _ret = snprintf((buf) + (n), (size) - (n), __VA_ARGS__); \
        if (_ret > 0) { \
            (n) += _ret; \
            if ((n) > (size)) { (n) = (size); } \
        } \
    } \
} while (0)

/* --- Base64 (RFC 4648, from zstreamer) --- */
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *in, int in_len, char *out)
{
    int i, o = 0;
    for (i = 0; i < in_len; i += 3) {
        int a = in[i];
        int b = (i + 1 < in_len) ? in[i + 1] : 0;
        int c = (i + 2 < in_len) ? in[i + 2] : 0;
        out[o++] = B64_CHARS[(a >> 2) & 0x3F];
        out[o++] = B64_CHARS[((a & 3) << 4) | ((b >> 4) & 0xF)];
        out[o++] = (i + 1 < in_len) ? B64_CHARS[((b & 0xF) << 2) | ((c >> 6) & 3)] : '=';
        out[o++] = (i + 2 < in_len) ? B64_CHARS[c & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}

/* --- avcC -> SDP params (from zstreamer) --- */
static int avcc_to_sdp_params(const uint8_t *extra, int extra_size,
                              char *profile_level_id, char *sprop, int sprop_cap)
{
    if (!extra || extra_size < 7 || extra[0] != 1) return 0;
    snprintf(profile_level_id, 7, "%02x%02x%02x", extra[1], extra[2], extra[3]);
    int p = 5;
    int num_sps = extra[p++] & 0x1F;
    int sprop_len = 0;
    char tmp[1024];
    for (int i = 0; i < num_sps && p + 2 <= extra_size; i++) {
        int nal_len = (extra[p] << 8) | extra[p + 1];
        p += 2;
        if (p + nal_len > extra_size) break;
        if (i > 0 && sprop_len + 1 < sprop_cap) sprop[sprop_len++] = ',';
        int enc_len = base64_encode(extra + p, nal_len, tmp);
        if (sprop_len + enc_len < sprop_cap) {
            memcpy(sprop + sprop_len, tmp, enc_len);
            sprop_len += enc_len;
        }
        p += nal_len;
    }
    if (p >= extra_size) { sprop[sprop_len] = '\0'; return 1; }
    int num_pps = extra[p++];
    for (int i = 0; i < num_pps && p + 2 <= extra_size; i++) {
        int nal_len = (extra[p] << 8) | extra[p + 1];
        p += 2;
        if (p + nal_len > extra_size) break;
        if (sprop_len + 1 < sprop_cap) sprop[sprop_len++] = ',';
        int enc_len = base64_encode(extra + p, nal_len, tmp);
        if (sprop_len + enc_len < sprop_cap) {
            memcpy(sprop + sprop_len, tmp, enc_len);
            sprop_len += enc_len;
        }
        p += nal_len;
    }
    sprop[sprop_len] = '\0';
    return 1;
}

/* --- SDP generation (from zstreamer make_sdp) --- */
static int make_sdp(rtsp_session_t *sess, char *out, int cap)
{
    int n = 0;
    uint64_t now = (uint64_t)time(NULL) + 2208988800ULL;
    APPEND_SNPRINTF(out, cap, n,
        "v=0\r\n"
        "o=- %llu %llu IN IP4 0.0.0.0\r\n"
        "s=%s\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "a=range:npt=0-\r\n"
        "a=sendonly\r\n"
        "a=control:*\r\n",
        (unsigned long long)now, (unsigned long long)now, sess->name);

    if (sess->has_video) {
        int pt = RTP_PT_H264;
        const char *enc = (sess->video_codec == CODEC_H265) ? "H265" : "H264";
        APPEND_SNPRINTF(out, cap, n,
            "m=video 0 RTP/AVP %d\r\n"
            "a=rtpmap:%d %s/%d\r\n",
            pt, pt, enc, RTP_CLOCK_VIDEO);
        if (sess->video_codec == CODEC_H264 && sess->extra_data && sess->extra_size > 0) {
            char plid[8] = "42e01f";
            char sprop[1024] = "";
            avcc_to_sdp_params(sess->extra_data, sess->extra_size,
                               plid, sprop, (int)sizeof(sprop));
            if (sprop[0] != '\0') {
                APPEND_SNPRINTF(out, cap, n,
                    "a=fmtp:%d packetization-mode=1;"
                    "profile-level-id=%s;"
                    "sprop-parameter-sets=%s\r\n"
                    "a=control:trackID=0\r\n",
                    pt, plid, sprop);
            } else {
                APPEND_SNPRINTF(out, cap, n,
                    "a=fmtp:%d packetization-mode=1;profile-level-id=%s\r\n"
                    "a=control:trackID=0\r\n",
                    pt, plid);
            }
        } else {
            APPEND_SNPRINTF(out, cap, n,
                "a=fmtp:%d packetization-mode=1;profile-level-id=42e01f\r\n"
                "a=control:trackID=0\r\n", pt);
        }
    }

    if (sess->has_audio) {
        int sr = sess->sample_rate > 0 ? sess->sample_rate : 44100;
        int ch = sess->channels > 0 ? sess->channels : 2;
        static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                     24000, 22050, 16000, 12000, 11025, 8000, 7350 };
        int freq_idx = 4;
        for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); i++) {
            if (rates[i] == sr) { freq_idx = i; break; }
        }
        if (ch < 1) ch = 1;
        if (ch > 7) ch = 2;
        int object_type = 2; /* AAC LC */
        uint8_t asc0 = (uint8_t)((object_type << 3) | (freq_idx >> 1));
        uint8_t asc1 = (uint8_t)(((freq_idx & 1) << 7) | (ch << 3));
        APPEND_SNPRINTF(out, cap, n,
            "m=audio 0 RTP/AVP %d\r\n"
            "a=rtpmap:%d MPEG4-GENERIC/%d/%d\r\n"
            "a=fmtp:%d streamtype=5;profile-level-id=1;"
            "mode=AAC-hbr;config=%02X%02X;"
            "sizelength=13;indexlength=3;indexdeltalength=3\r\n"
            "a=control:trackID=1\r\n",
            RTP_PT_AAC, RTP_PT_AAC, sr, ch, RTP_PT_AAC, asc0, asc1);
    }
    return n;
}

/* --- RTSP reply helpers --- */
static const char *reason_phrase(int code)
{
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 454: return "Session Not Found";
        case 461: return "Unsupported Transport";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "RTSP Version Not Supported";
        default:  return "Unknown";
    }
}

static int send_reply(rtsp_client_t *cl, int code,
                      const char *extra_hdrs, const char *body, int body_len)
{
    char reply[RTSP_REPLY_SIZE];
    time_t t = time(NULL);
    struct tm tm; gmtime_r(&t, &tm);
    char date[64]; strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    int n = 0;
    APPEND_SNPRINTF(reply, sizeof(reply), n,
        "RTSP/1.0 %d %s\r\n"
        "CSeq: %u\r\n"
        "Date: %s\r\n"
        "Server: zff/1.0\r\n",
        code, reason_phrase(code), cl->cseq, date);
    if (cl->session_id[0])
        APPEND_SNPRINTF(reply, sizeof(reply), n, "Session: %s\r\n", cl->session_id);
    if (extra_hdrs)
        APPEND_SNPRINTF(reply, sizeof(reply), n, "%s", extra_hdrs);
    if (body && body_len > 0)
        APPEND_SNPRINTF(reply, sizeof(reply), n, "Content-Length: %d\r\n", body_len);
    APPEND_SNPRINTF(reply, sizeof(reply), n, "\r\n");
    if (send(cl->fd, reply, n, MSG_NOSIGNAL) != n) return -1;
    if (body && body_len > 0) {
        if (send(cl->fd, body, body_len, MSG_NOSIGNAL) != body_len) return -1;
    }
    return 0;
}

static int reply_simple(rtsp_client_t *cl, int code)
{
    return send_reply(cl, code, NULL, NULL, 0);
}

/* --- RTSP request parsing (from zstreamer) --- */
static int parse_rtsp_request(rtsp_client_t *cl)
{
    char *hdr_end = strstr(cl->buf, "\r\n\r\n");
    if (!hdr_end) {
        if (cl->buf_len >= (int)sizeof(cl->buf) - 1) return 400;
        return 1; /* need more data */
    }
    int hdr_len = (int)(hdr_end - cl->buf) + 4;

    char *line_end = strstr(cl->buf, "\r\n");
    if (!line_end) return 400;
    *line_end = '\0';
    char version[16] = "";
    if (sscanf(cl->buf, "%15s %511s %15s", cl->method, cl->uri, version) != 3) return 400;
    if (strcmp(version, "RTSP/1.0") != 0) return 505;

    cl->cseq = 0;
    cl->transport_hdr[0] = '\0';
    char *p = line_end + 2;
    while (p < hdr_end) {
        char *eol = strstr(p, "\r\n");
        if (!eol) break;
        *eol = '\0';
        if (strncasecmp(p, "CSeq:", 5) == 0) cl->cseq = (unsigned)atoi(p + 5);
        else if (strncasecmp(p, "Session:", 8) == 0) {
            const char *v = p + 8;
            while (*v == ' ') v++;
            snprintf(cl->session_id, sizeof(cl->session_id), "%31s", v);
            char *semi = strchr(cl->session_id, ';');
            if (semi) *semi = '\0';
        } else if (strncasecmp(p, "Transport:", 10) == 0) {
            const char *v = p + 10;
            while (*v == ' ') v++;
            snprintf(cl->transport_hdr, sizeof(cl->transport_hdr), "%255s", v);
        }
        p = eol + 2;
    }

    int remain = cl->buf_len - hdr_len;
    if (remain > 0) memmove(cl->buf, cl->buf + hdr_len, remain);
    cl->buf_len = remain;
    cl->buf[remain] = '\0';
    return 0;
}

static int extract_mount_clean(const char *uri, char *out, int max_len)
{
    const char *path = strstr(uri, "://");
    if (path) {
        path = strchr(path + 3, '/');
        if (!path) return 0;
    } else {
        path = uri;
    }
    path += strspn(path, "/");
    int i = 0;
    while (*path && *path != '/' && *path != '?' && i + 1 < max_len) {
        out[i++] = *path++;
    }
    out[i] = '\0';
    return i > 0;
}

/* --- Transport header parsing (from zstreamer) --- */
static void parse_transport_token(const char *tok, size_t tok_len,
                                  int *transport_type,
                                  uint16_t *cport1, uint16_t *cport2,
                                  uint16_t *port1, uint16_t *port2,
                                  int *interleaved1, int *interleaved2,
                                  int *multicast, char *destination, int *ttl)
{
    char buf[128];
    if (tok_len >= sizeof(buf)) tok_len = sizeof(buf) - 1;
    memcpy(buf, tok, tok_len);
    buf[tok_len] = '\0';

    if (tok_len == 11 && strncasecmp(tok, "RTP/AVP/TCP", 11) == 0)
        *transport_type = RTSP_TRANSPORT_TCP;
    else if (tok_len == 11 && strncasecmp(tok, "RTP/AVP/UDP", 11) == 0)
        *transport_type = RTSP_TRANSPORT_UDP;
    else if (tok_len == 7 && strncasecmp(tok, "RTP/AVP", 7) == 0)
        *transport_type = RTSP_TRANSPORT_UDP;
    else if (strncasecmp(buf, "unicast", 7) == 0) { }
    else if (strncasecmp(buf, "multicast", 9) == 0) *multicast = 1;
    else if (tok_len > 12 && strncasecmp(tok, "interleaved=", 12) == 0) {
        if (sscanf(buf + 12, "%d-%d", interleaved1, interleaved2) >= 1) {
            if (*interleaved2 < 0) *interleaved2 = *interleaved1 + 1;
        }
    } else if (tok_len > 12 && strncasecmp(tok, "client_port=", 12) == 0) {
        int a = 0, b = 0;
        if (sscanf(buf + 12, "%d-%d", &a, &b) >= 1) {
            *cport1 = (uint16_t)a;
            *cport2 = (uint16_t)(b > 0 ? b : a + 1);
        }
    } else if (tok_len > 5 && strncasecmp(tok, "port=", 5) == 0) {
        int a = 0, b = 0;
        if (sscanf(buf + 5, "%d-%d", &a, &b) >= 1) {
            *port1 = (uint16_t)a;
            *port2 = (uint16_t)(b > 0 ? b : a + 1);
        }
    } else if (tok_len > 12 && strncasecmp(tok, "destination=", 12) == 0) {
        snprintf(destination, 64, "%63s", buf + 12);
    } else if (tok_len > 4 && strncasecmp(tok, "ttl=", 4) == 0) {
        *ttl = atoi(buf + 4);
    }
}

static void parse_transport_header(const char *field,
                                   int *transport_type,
                                   uint16_t *cport1, uint16_t *cport2,
                                   uint16_t *port1, uint16_t *port2,
                                   int *interleaved1, int *interleaved2,
                                   int *multicast, char *destination, int *ttl)
{
    *transport_type = RTSP_TRANSPORT_TCP;
    *cport1 = *cport2 = 0;
    *port1 = *port2 = 0;
    *interleaved1 = *interleaved2 = -1;
    *multicast = 0;
    destination[0] = '\0';
    *ttl = 0;
    const char *p = field;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        parse_transport_token(p, len, transport_type, cport1, cport2,
                              port1, port2, interleaved1, interleaved2,
                              multicast, destination, ttl);
        p += len;
    }
    if (*interleaved1 < 0 && *transport_type == RTSP_TRANSPORT_TCP) {
        *interleaved1 = 0;
        *interleaved2 = 1;
    }
}

/* --- UDP socket helpers --- */
static int create_udp_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    return fd;
}

static int bind_udp_ephemeral(int fd, uint16_t *out_port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return -1;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) return -1;
    *out_port = ntohs(addr.sin_port);
    return 0;
}

/* --- RTP send: TCP interleaved or UDP unicast --- */
static void write_rtp_packet(rtsp_client_t *cl, rtp_stream_t *st,
                             const uint8_t *data, int len)
{
    if (cl->transport_type == RTSP_TRANSPORT_UDP && st->udp_rtp_fd >= 0) {
        sendto(st->udp_rtp_fd, data, len, 0,
               (struct sockaddr *)&st->client_rtp_addr,
               sizeof(st->client_rtp_addr));
    } else {
        uint8_t frame[4];
        frame[0] = '$';
        frame[1] = (uint8_t)st->interleaved_ch;
        frame[2] = (uint8_t)((len >> 8) & 0xff);
        frame[3] = (uint8_t)(len & 0xff);
        struct iovec iov[2] = {
            { .iov_base = frame, .iov_len = 4 },
            { .iov_base = (void *)data, .iov_len = (size_t)len }
        };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;
        sendmsg(cl->fd, &msg, MSG_NOSIGNAL);
    }
}

/* --- RTCP Sender Report (from zstreamer) --- */
static int send_rtcp_sr(rtsp_client_t *cl, int is_video)
{
    rtp_stream_t *st = is_video ? &cl->vstream : &cl->astream;
    if (!st->codec || !st->packet_count) return 0;

    uint8_t buf[28];
    buf[0] = 0x80; buf[1] = 200;
    AV_WB16(buf + 2, 6);
    AV_WB32(buf + 4, st->ssrc);
    uint64_t ntp = ntp_now();
    AV_WB32(buf + 8, (uint32_t)(ntp >> 32));
    AV_WB32(buf + 12, (uint32_t)(ntp & 0xffffffff));
    AV_WB32(buf + 16, st->last_rtp_ts);
    AV_WB32(buf + 20, st->packet_count);
    AV_WB32(buf + 24, st->octet_count);

    int slen = (int)sizeof(buf);
    if (cl->transport_type == RTSP_TRANSPORT_UDP && st->udp_rtcp_fd >= 0) {
        int n = sendto(st->udp_rtcp_fd, buf, slen, 0,
                       (struct sockaddr *)&st->client_rtcp_addr,
                       sizeof(st->client_rtcp_addr));
        return (n == slen) ? 0 : -1;
    }
    uint8_t frame[4 + 28];
    frame[0] = '$';
    frame[1] = (uint8_t)(st->interleaved_ch + 1);
    frame[2] = (uint8_t)((slen >> 8) & 0xff);
    frame[3] = (uint8_t)(slen & 0xff);
    memcpy(frame + 4, buf, slen);
    return (send(cl->fd, frame, 4 + slen, MSG_NOSIGNAL) == 4 + slen) ? 0 : -1;
}

/* --- RTSP method handlers --- */
static int on_options(rtsp_client_t *cl)
{
    return send_reply(cl, 200,
        "Public: DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, OPTIONS\r\n",
        NULL, 0);
}

static int on_describe(rtsp_client_t *cl)
{
    rtsp_server_t *srv = cl->server;
    char mount[128];
    if (!extract_mount_clean(cl->uri, mount, sizeof(mount))) return reply_simple(cl, 404);
    if (strcmp(mount, srv->session.name) != 0) return reply_simple(cl, 404);
    cl->session = &srv->session;

    char sdp[RTSP_SDP_SIZE];
    int sdp_len = make_sdp(&srv->session, sdp, sizeof(sdp));

    char base_url[1024];
    if (strstr(cl->uri, "://")) {
        size_t uri_len = strlen(cl->uri);
        if (uri_len > 0 && cl->uri[uri_len - 1] == '/')
            snprintf(base_url, sizeof(base_url), "%s", cl->uri);
        else
            snprintf(base_url, sizeof(base_url), "%s/", cl->uri);
    } else {
        struct sockaddr_in local_addr;
        socklen_t ll = sizeof(local_addr);
        char local_ip[64] = "127.0.0.1";
        uint16_t local_port = srv->port;
        if (getsockname(cl->fd, (struct sockaddr *)&local_addr, &ll) == 0) {
            inet_ntop(AF_INET, &local_addr.sin_addr, local_ip, sizeof(local_ip));
            local_port = ntohs(local_addr.sin_port);
        }
        const char *path = cl->uri + strspn(cl->uri, "/");
        snprintf(base_url, sizeof(base_url), "rtsp://%s:%d/%s/", local_ip, local_port, path);
    }

    char extras[1200];
    snprintf(extras, sizeof(extras),
        "Content-Type: application/sdp\r\n"
        "Content-Base: %s\r\n",
        base_url);
    return send_reply(cl, 200, extras, sdp, sdp_len);
}

static int on_setup(rtsp_client_t *cl)
{
    rtsp_server_t *srv = cl->server;
    if (!cl->session) {
        char mount[128];
        if (!extract_mount_clean(cl->uri, mount, sizeof(mount))) return reply_simple(cl, 454);
        if (strcmp(mount, srv->session.name) != 0) return reply_simple(cl, 454);
        cl->session = &srv->session;
    }
    rtsp_session_t *sess = cl->session;

    const char *track = strstr(cl->uri, "trackID=");
    int is_video_track = 0, is_audio_track = 0;
    if (track) {
        int tid = atoi(track + 8);
        if (tid == 0) is_video_track = 1;
        else if (tid == 1) is_audio_track = 1;
    } else {
        if (sess->has_video) is_video_track = 1;
        else if (sess->has_audio) is_audio_track = 1;
    }
    if ((is_video_track && !sess->has_video) || (is_audio_track && !sess->has_audio))
        return reply_simple(cl, 454);

    int transport_type_parsed = RTSP_TRANSPORT_TCP;
    uint16_t cport1 = 0, cport2 = 0, port1 = 0, port2 = 0;
    int il1 = -1, il2 = -1, multicast = 0, ttl = 0;
    char destination[64] = "";
    int has_transport = 0;
    if (cl->transport_hdr[0]) {
        has_transport = 1;
        parse_transport_header(cl->transport_hdr, &transport_type_parsed,
                               &cport1, &cport2, &port1, &port2,
                               &il1, &il2, &multicast, destination, &ttl);
    }
    if (srv->force_tcp) {
        transport_type_parsed = RTSP_TRANSPORT_TCP;
        cport1 = cport2 = 0;
        multicast = 0;
    }
    if (multicast) return reply_simple(cl, 461); /* v1: no multicast */

    if (cl->track_setup_mask == 0) {
        snprintf(cl->session_id, sizeof(cl->session_id), "%08x", rand32());
        if (transport_type_parsed == RTSP_TRANSPORT_TCP ||
            (transport_type_parsed == RTSP_TRANSPORT_UDP && cport1 == 0 && has_transport)) {
            cl->transport_type = RTSP_TRANSPORT_TCP;
            cl->interleaved_rtp  = (il1 >= 0) ? il1 : 0;
            cl->interleaved_rtcp = (il2 >= 0) ? il2 : cl->interleaved_rtp + 1;
        } else if (transport_type_parsed == RTSP_TRANSPORT_UDP && cport1 > 0) {
            cl->transport_type = RTSP_TRANSPORT_UDP;
        } else {
            cl->transport_type = RTSP_TRANSPORT_TCP;
            cl->interleaved_rtp  = 0;
            cl->interleaved_rtcp = 1;
        }
    } else if (cl->transport_type == RTSP_TRANSPORT_TCP && il1 >= 0) {
        /* Second track on TCP: honor explicit interleaved channels */
        cl->interleaved_rtp = il1;
        if (il2 >= 0) cl->interleaved_rtcp = il2;
    }

    if (is_video_track && sess->has_video && !(cl->track_setup_mask & 1)) {
        memset(&cl->vstream, 0, sizeof(cl->vstream));
        cl->vstream.udp_rtp_fd = cl->vstream.udp_rtcp_fd = -1;
        cl->vstream.ssrc = rand32();
        cl->vstream.payload_type = RTP_PT_H264;
        cl->vstream.clock_rate = RTP_CLOCK_VIDEO;
        cl->vstream.interleaved_ch = cl->interleaved_rtp;
        cl->vstream.codec = sess->video_codec;
        cl->track_setup_mask |= 1;
    }
    if (is_audio_track && sess->has_audio && !(cl->track_setup_mask & 2)) {
        memset(&cl->astream, 0, sizeof(cl->astream));
        cl->astream.udp_rtp_fd = cl->astream.udp_rtcp_fd = -1;
        cl->astream.ssrc = rand32();
        cl->astream.payload_type = RTP_PT_AAC;
        cl->astream.clock_rate = sess->sample_rate > 0 ? sess->sample_rate : 44100;
        cl->astream.interleaved_ch = (il1 >= 0) ? il1 : cl->interleaved_rtp + 2;
        cl->astream.codec = CODEC_AAC;
        cl->track_setup_mask |= 2;
    }

    if (cl->transport_type == RTSP_TRANSPORT_UDP && cport1 > 0) {
        rtp_stream_t *st = is_audio_track ? &cl->astream : &cl->vstream;
        st->udp_rtp_fd = create_udp_socket();
        st->udp_rtcp_fd = create_udp_socket();
        if (st->udp_rtp_fd < 0 || st->udp_rtcp_fd < 0) {
            if (st->udp_rtp_fd >= 0) close(st->udp_rtp_fd);
            if (st->udp_rtcp_fd >= 0) close(st->udp_rtcp_fd);
            st->udp_rtp_fd = st->udp_rtcp_fd = -1;
            return reply_simple(cl, 500);
        }
        if (bind_udp_ephemeral(st->udp_rtp_fd, &st->server_rtp_port) < 0 ||
            bind_udp_ephemeral(st->udp_rtcp_fd, &st->server_rtcp_port) < 0) {
            close(st->udp_rtp_fd);
            close(st->udp_rtcp_fd);
            st->udp_rtp_fd = st->udp_rtcp_fd = -1;
            return reply_simple(cl, 500);
        }
        memset(&st->client_rtp_addr, 0, sizeof(st->client_rtp_addr));
        st->client_rtp_addr.sin_family = AF_INET;
        st->client_rtp_addr.sin_addr.s_addr = inet_addr(cl->peer_ip);
        st->client_rtp_addr.sin_port = htons(cport1);
        memset(&st->client_rtcp_addr, 0, sizeof(st->client_rtcp_addr));
        st->client_rtcp_addr.sin_family = AF_INET;
        st->client_rtcp_addr.sin_addr.s_addr = inet_addr(cl->peer_ip);
        st->client_rtcp_addr.sin_port = htons(cport2);
        st->client_rtp_port = cport1;
        st->client_rtcp_port = cport2;
    }

    char extra[256];
    if (cl->transport_type == RTSP_TRANSPORT_UDP) {
        rtp_stream_t *st = is_audio_track ? &cl->astream : &cl->vstream;
        snprintf(extra, sizeof(extra),
            "Transport: RTP/AVP/UDP;unicast;"
            "client_port=%hu-%hu;"
            "server_port=%hu-%hu\r\n",
            st->client_rtp_port, st->client_rtcp_port,
            st->server_rtp_port, st->server_rtcp_port);
    } else {
        rtp_stream_t *st = is_audio_track ? &cl->astream : &cl->vstream;
        snprintf(extra, sizeof(extra),
            "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n",
            st->interleaved_ch, st->interleaved_ch + 1);
    }
    return send_reply(cl, 200, extra, NULL, 0);
}

static int on_play(rtsp_client_t *cl)
{
    if (!cl->session_id[0]) return reply_simple(cl, 454);
    rtsp_server_t *srv = cl->server;

    /* Create per-client payloaders (own seq spaces) before replying */
    if ((cl->track_setup_mask & 1) && cl->vstream.codec && !cl->vstream.payloader) {
        zstr_rtp_codec_t rc = (cl->vstream.codec == CODEC_H265)
                              ? ZSTR_RTP_CODEC_H265 : ZSTR_RTP_CODEC_H264;
        cl->vstream.payloader = zstr_rtp_payloader_create(
            rc, cl->vstream.payload_type, cl->vstream.ssrc,
            cl->vstream.clock_rate, srv->mtu);
        if (!cl->vstream.payloader) return reply_simple(cl, 500);
    }
    if ((cl->track_setup_mask & 2) && cl->astream.codec && !cl->astream.payloader) {
        cl->astream.payloader = zstr_rtp_payloader_create(
            ZSTR_RTP_CODEC_AAC, cl->astream.payload_type, cl->astream.ssrc,
            cl->astream.clock_rate, srv->mtu);
        if (!cl->astream.payloader) return reply_simple(cl, 500);
    }

    char extra[512];
    int n = 0;
    APPEND_SNPRINTF(extra, sizeof(extra), n, "Range: npt=0.000-\r\nRTP-Info: ");
    int first = 1;
    size_t uri_len = strlen(cl->uri);
    const char *slash = (uri_len > 0 && cl->uri[uri_len - 1] == '/') ? "" : "/";
    if (cl->track_setup_mask & 1) {
        APPEND_SNPRINTF(extra, sizeof(extra), n,
            "url=%s%strackID=0;seq=%u",
            cl->uri, slash, (unsigned)zstr_rtp_payloader_seq(cl->vstream.payloader));
        first = 0;
    }
    if (cl->track_setup_mask & 2) {
        if (!first) APPEND_SNPRINTF(extra, sizeof(extra), n, ",");
        APPEND_SNPRINTF(extra, sizeof(extra), n,
            "url=%s%strackID=1;seq=%u",
            cl->uri, slash, (unsigned)zstr_rtp_payloader_seq(cl->astream.payloader));
    }
    APPEND_SNPRINTF(extra, sizeof(extra), n, "\r\n");

    int ret = send_reply(cl, 200, extra, NULL, 0);
    cl->play_state = 1;
    return ret;
}

static int on_pause(rtsp_client_t *cl)
{
    if (!cl->session_id[0]) return reply_simple(cl, 454);
    cl->play_state = 2;
    return reply_simple(cl, 200);
}

static int on_teardown(rtsp_client_t *cl)
{
    cl->play_state = 0;
    return reply_simple(cl, 200);
}

static int dispatch_rtsp(rtsp_client_t *cl)
{
    if      (strcasecmp(cl->method, "OPTIONS")  == 0) return on_options(cl);
    else if (strcasecmp(cl->method, "DESCRIBE") == 0) return on_describe(cl);
    else if (strcasecmp(cl->method, "SETUP")    == 0) return on_setup(cl);
    else if (strcasecmp(cl->method, "PLAY")     == 0) return on_play(cl);
    else if (strcasecmp(cl->method, "PAUSE")    == 0) return on_pause(cl);
    else if (strcasecmp(cl->method, "TEARDOWN") == 0) return on_teardown(cl);
    return reply_simple(cl, 501);
}

/* --- Annex-B helpers (split AU into NALs, mirroring zstreamer scan) --- */
static int annexb_next_nal(const uint8_t *d, int sz, int *pos,
                           const uint8_t **nal, int *nal_len)
{
    int i = *pos;
    while (i + 2 < sz) {
        if (d[i] == 0 && d[i + 1] == 0) {
            int ns, clen;
            if (i + 3 < sz && d[i + 2] == 1) { ns = i + 3; clen = 3; }
            else if (i + 4 < sz && d[i + 2] == 0 && d[i + 3] == 1) { ns = i + 4; clen = 4; }
            else { i++; continue; }
            int ne = sz;
            for (int j = ns; j + 3 < sz; j++) {
                if (d[j] == 0 && d[j + 1] == 0 &&
                    (d[j + 2] == 1 || (j + 4 < sz && d[j + 2] == 0 && d[j + 3] == 1))) {
                    ne = j;
                    break;
                }
            }
            /* Skip zero-length (consecutive start codes) */
            if (ne - ns <= 0) { i = ne; continue; }
            *nal = d + ns;
            *nal_len = ne - ns;
            *pos = ne;
            (void)clen;
            return 1;
        }
        i++;
    }
    /* Trailing data without start code: treat as one NAL (e.g. AVCC/raw) */
    if (*pos < sz && *pos == 0) {
        *nal = d + *pos;
        *nal_len = sz - *pos;
        *pos = sz;
        return 1;
    }
    return 0;
}

/* Cache unseen H264 SPS/PPS from Annex-B data */
static void cache_sps_pps(rtsp_session_t *sess, const uint8_t *d, int sz)
{
    int pos = 0;
    const uint8_t *nal;
    int nal_len;
    while (annexb_next_nal(d, sz, &pos, &nal, &nal_len)) {
        uint8_t nt = nal[0] & 0x1f;
        if (nt != H264_NAL_SPS && nt != H264_NAL_PPS) continue;
        /* dedup */
        int found = 0;
        int cpos = 0;
        while (cpos + 4 <= sess->sps_pps_cache_size) {
            if (sess->sps_pps_cache[cpos] == 0 && sess->sps_pps_cache[cpos + 1] == 0 &&
                sess->sps_pps_cache[cpos + 2] == 0 && sess->sps_pps_cache[cpos + 3] == 1) {
                int ns = cpos + 4, ne = sess->sps_pps_cache_size, j;
                for (j = ns; j + 3 < sess->sps_pps_cache_size; j++) {
                    if (sess->sps_pps_cache[j] == 0 && sess->sps_pps_cache[j + 1] == 0 &&
                        (sess->sps_pps_cache[j + 2] == 1 ||
                         (j + 4 < sess->sps_pps_cache_size &&
                          sess->sps_pps_cache[j + 2] == 0 && sess->sps_pps_cache[j + 3] == 1))) {
                        ne = j;
                        break;
                    }
                }
                if (ne - ns == nal_len && memcmp(sess->sps_pps_cache + ns, nal, nal_len) == 0) {
                    found = 1;
                    break;
                }
                cpos = ne;
            } else {
                cpos++;
            }
        }
        if (!found) {
            uint8_t *nc = realloc(sess->sps_pps_cache, sess->sps_pps_cache_size + 4 + nal_len);
            if (nc) {
                sess->sps_pps_cache = nc;
                nc[sess->sps_pps_cache_size + 0] = 0;
                nc[sess->sps_pps_cache_size + 1] = 0;
                nc[sess->sps_pps_cache_size + 2] = 0;
                nc[sess->sps_pps_cache_size + 3] = 1;
                memcpy(nc + sess->sps_pps_cache_size + 4, nal, nal_len);
                sess->sps_pps_cache_size += 4 + nal_len;
            }
        }
    }
}

/* --- Fan-out: deliver one AVPacket to all PLAYing clients ---
 * Video Annex-B splitting, marker handling and AAC RFC 3640 framing are
 * all done inside zstr_rtp_payloader now; this layer only fans out. */
static void deliver_video(rtsp_server_t *srv, rtsp_client_t *cl,
                          rtp_stream_t *st, const AVPacket *pkt)
{
    const uint8_t *d = pkt->data;
    int sz = pkt->size;

    if (srv->session.video_codec == CODEC_H264)
        cache_sps_pps(&srv->session, d, sz);

    /* Collect RTP packets for cached SPS/PPS first (late joiners whose
     * frames lack parameter sets), then the current access unit. */
    AVPacket **all = NULL;
    int nb_all = 0;

    if (!st->sps_pps_sent && srv->session.sps_pps_cache &&
        srv->session.sps_pps_cache_size > 0) {
        AVPacket tmp;
        av_init_packet(&tmp);
        tmp.data = srv->session.sps_pps_cache;
        tmp.size = srv->session.sps_pps_cache_size;
        tmp.pts = pkt->pts;
        tmp.dts = pkt->dts;
        tmp.duration = pkt->duration;
        tmp.time_base = pkt->time_base;
        AVPacket **out = NULL;
        int nb_out = 0;
        if (zstr_rtp_payloader_process(st->payloader, &tmp, &out, &nb_out) == 0) {
            /* Cached parameter sets are a prefix of this AU: keep M=0 so the
             * whole burst (SPS/PPS + frame) reassembles as one access unit. */
            for (int k = 0; k < nb_out; k++) {
                if (out[k]->size > 1) out[k]->data[1] &= ~0x80;
            }
            AVPacket **merged = realloc(all, (nb_all + nb_out) * sizeof(AVPacket *));
            if (merged) {
                all = merged;
                memcpy(all + nb_all, out, nb_out * sizeof(AVPacket *));
                nb_all += nb_out;
            } else {
                zstr_rtp_payloader_free_packets(out, nb_out);
            }
            free(out);
        }
        st->sps_pps_sent = 1;
    }

    AVPacket **out = NULL;
    int nb_out = 0;
    if (zstr_rtp_payloader_process(st->payloader, pkt, &out, &nb_out) == 0) {
        AVPacket **merged = realloc(all, (nb_all + nb_out) * sizeof(AVPacket *));
        if (merged) {
            all = merged;
            memcpy(all + nb_all, out, nb_out * sizeof(AVPacket *));
            nb_all += nb_out;
        } else {
            zstr_rtp_payloader_free_packets(out, nb_out);
        }
        free(out);
    }

    for (int i = 0; i < nb_all; i++) {
        if (all[i]->size >= 12)
            st->last_rtp_ts = ntohl(*(uint32_t *)(all[i]->data + 4));
        write_rtp_packet(cl, st, all[i]->data, all[i]->size);
        st->packet_count++;
        st->octet_count += all[i]->size - 12;
    }
    zstr_rtp_payloader_free_packets(all, nb_all);
}

static void deliver_audio(rtsp_client_t *cl, rtp_stream_t *st, const AVPacket *pkt)
{
    AVPacket **out = NULL;
    int nb_out = 0;
    if (zstr_rtp_payloader_process(st->payloader, pkt, &out, &nb_out) < 0) return;
    for (int i = 0; i < nb_out; i++) {
        if (out[i]->size >= 12)
            st->last_rtp_ts = ntohl(*(uint32_t *)(out[i]->data + 4));
        write_rtp_packet(cl, st, out[i]->data, out[i]->size);
        st->packet_count++;
        st->octet_count += out[i]->size - 12;
    }
    zstr_rtp_payloader_free_packets(out, nb_out);
}

static void server_push_packet(rtsp_server_t *srv, const AVPacket *pkt,
                               AVRational tb, int is_video)
{
    if (!pkt || !pkt->data || pkt->size <= 0) return;

    AVPacket cpy;
    av_init_packet(&cpy);
    cpy.data = pkt->data;
    cpy.size = pkt->size;
    cpy.pts = pkt->pts;
    cpy.dts = pkt->dts;
    cpy.duration = pkt->duration;
    cpy.time_base = tb;

    pthread_mutex_lock(&srv->lock);
    for (rtsp_client_t *cl = srv->clients; cl; cl = cl->next) {
        if (cl->play_state != 1 || cl->session != &srv->session) continue;
        if (is_video) {
            if (!(cl->track_setup_mask & 1) || !cl->vstream.payloader) continue;
            deliver_video(srv, cl, &cl->vstream, &cpy);
        } else {
            if (!(cl->track_setup_mask & 2) || !cl->astream.payloader) continue;
            deliver_audio(cl, &cl->astream, &cpy);
        }
    }
    pthread_mutex_unlock(&srv->lock);
}

/* --- Client / listen threads --- */
static void client_free_stream(rtp_stream_t *st)
{
    if (st->payloader) zstr_rtp_payloader_free(&st->payloader);
    if (st->udp_rtp_fd >= 0) close(st->udp_rtp_fd);
    if (st->udp_rtcp_fd >= 0) close(st->udp_rtcp_fd);
    st->udp_rtp_fd = st->udp_rtcp_fd = -1;
}

static void *client_thread(void *arg)
{
    rtsp_client_t *cl = (rtsp_client_t *)arg;
    cl->running = 1;
    struct timespec last_rtcp;
    clock_gettime(CLOCK_MONOTONIC, &last_rtcp);

    while (cl->running) {
        struct pollfd pfd = { .fd = cl->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 1000);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - last_rtcp.tv_sec) * 1000 +
                          (now.tv_nsec - last_rtcp.tv_nsec) / 1000000;
        if (cl->play_state == 1 && elapsed_ms > RTCP_INTERVAL_MS) {
            last_rtcp = now;
            rtsp_server_t *srv = cl->server;
            pthread_mutex_lock(&srv->lock);
            if (cl->vstream.codec) send_rtcp_sr(cl, 1);
            if (cl->astream.codec) send_rtcp_sr(cl, 0);
            pthread_mutex_unlock(&srv->lock);
        }

        if (ret == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = read(cl->fd, cl->buf + cl->buf_len, sizeof(cl->buf) - cl->buf_len - 1);
        if (n <= 0) break;
        cl->buf_len += (int)n;
        cl->buf[cl->buf_len] = '\0';

        while (cl->buf_len > 0) {
            if ((uint8_t)cl->buf[0] == '$') {
                if (cl->buf_len < 4) break;
                int dlen = ((uint8_t)cl->buf[2] << 8) | (uint8_t)cl->buf[3];
                if (cl->buf_len < 4 + dlen) break;
                memmove(cl->buf, cl->buf + 4 + dlen, cl->buf_len - 4 - dlen);
                cl->buf_len -= 4 + dlen;
                continue;
            }
            int r = parse_rtsp_request(cl);
            if (r == 1) break;
            if (r != 0) { reply_simple(cl, r); break; }
            rtsp_server_t *srv = cl->server;
            pthread_mutex_lock(&srv->lock);
            dispatch_rtsp(cl);
            pthread_mutex_unlock(&srv->lock);
        }
    }

    if (cl->server) {
        pthread_mutex_lock(&cl->server->lock);
        rtsp_client_t **pp = &cl->server->clients;
        while (*pp) {
            if (*pp == cl) { *pp = cl->next; cl->server->client_count--; break; }
            pp = &(*pp)->next;
        }
        pthread_mutex_unlock(&cl->server->lock);
    }
    close(cl->fd);
    client_free_stream(&cl->vstream);
    client_free_stream(&cl->astream);
    free(cl);
    return NULL;
}

static void *listen_thread(void *arg)
{
    rtsp_server_t *srv = (rtsp_server_t *)arg;
    while (srv->running) {
        struct pollfd pfd = { .fd = srv->listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) continue;

        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(srv->listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) continue;

        pthread_mutex_lock(&srv->lock);
        int over_limit = (srv->client_count >= MAX_CLIENTS);
        pthread_mutex_unlock(&srv->lock);
        if (over_limit) { close(fd); continue; }

        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        rtsp_client_t *cl = calloc(1, sizeof(*cl));
        if (!cl) { close(fd); continue; }
        cl->fd = fd;
        inet_ntop(AF_INET, &addr.sin_addr, cl->peer_ip, sizeof(cl->peer_ip));
        cl->peer_port = ntohs(addr.sin_port);
        cl->server = srv;
        cl->transport_type = RTSP_TRANSPORT_TCP;
        cl->interleaved_rtp = 0;
        cl->interleaved_rtcp = 1;
        cl->vstream.udp_rtp_fd = cl->vstream.udp_rtcp_fd = -1;
        cl->astream.udp_rtp_fd = cl->astream.udp_rtcp_fd = -1;

        pthread_mutex_lock(&srv->lock);
        cl->next = srv->clients;
        srv->clients = cl;
        srv->client_count++;
        pthread_mutex_unlock(&srv->lock);

        pthread_t th;
        pthread_create(&th, NULL, client_thread, cl);
        pthread_detach(th);
    }
    return NULL;
}

/* --- Server lifecycle --- */
static rtsp_server_t *server_create(int port, const char *mount, int force_tcp, int mtu)
{
    rtsp_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;
    srv->listen_fd = -1;
    srv->port = port > 0 ? port : ZSTR_RTSP_DEFAULT_PORT;
    snprintf(srv->mount, sizeof(srv->mount), "%s", mount && mount[0] ? mount : "live");
    snprintf(srv->session.name, sizeof(srv->session.name), "%s", srv->mount);
    srv->force_tcp = force_tcp;
    srv->mtu = (mtu >= 100 && mtu <= 65500) ? mtu : ZSTR_RTSP_DEFAULT_MTU;
    pthread_mutex_init(&srv->lock, NULL);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) goto fail;
    int reuse = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)srv->port);
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
    /* If port was 0, read back the ephemeral port */
    socklen_t alen = sizeof(addr);
    if (getsockname(srv->listen_fd, (struct sockaddr *)&addr, &alen) == 0)
        srv->port = ntohs(addr.sin_port);
    if (listen(srv->listen_fd, 8) < 0) goto fail;

    srv->running = 1;
    if (pthread_create(&srv->listen_thread, NULL, listen_thread, srv) != 0) goto fail;
    return srv;

fail:
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    pthread_mutex_destroy(&srv->lock);
    free(srv);
    return NULL;
}

static void server_free(rtsp_server_t **psrv)
{
    if (!psrv || !*psrv) return;
    rtsp_server_t *srv = *psrv;
    srv->running = 0;
    pthread_join(srv->listen_thread, NULL);
    if (srv->listen_fd >= 0) {
        shutdown(srv->listen_fd, SHUT_RDWR);
        close(srv->listen_fd);
    }
    /* Wake + reap clients: closing fds breaks their poll/read */
    pthread_mutex_lock(&srv->lock);
    rtsp_client_t *cl = srv->clients;
    while (cl) {
        cl->running = 0;
        shutdown(cl->fd, SHUT_RDWR);
        cl = cl->next;
    }
    pthread_mutex_unlock(&srv->lock);
    /* Give detached threads a moment to unlink themselves */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000 };
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&srv->lock);
    cl = srv->clients;
    while (cl) {
        rtsp_client_t *nx = cl->next;
        close(cl->fd);
        client_free_stream(&cl->vstream);
        client_free_stream(&cl->astream);
        free(cl);
        cl = nx;
    }
    pthread_mutex_unlock(&srv->lock);
    pthread_mutex_destroy(&srv->lock);
    free(srv->session.extra_data);
    free(srv->session.sps_pps_cache);
    free(srv);
    *psrv = NULL;
}

/* --- FFmpeg muxer wrapper --- */
typedef struct RTSMuxContext {
    const AVClass *av_class;
    int port;
    char *mount;
    int force_tcp;
    int mtu;
    rtsp_server_t *server;
} RTSMuxContext;

#define OFFSET(x) offsetof(RTSMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_rtspserver_options[] = {
    { "port",      "RTSP listen port",              OFFSET(port),      AV_OPT_TYPE_INT,    { .i64 = 8554 }, 0, 65535, ENC },
    { "mount",     "Mount point name",              OFFSET(mount),     AV_OPT_TYPE_STRING, { .str = "live" }, 0, 0, ENC },
    { "force_tcp", "Force TCP interleaved, reject UDP", OFFSET(force_tcp), AV_OPT_TYPE_INT, { .i64 = 0 },   0, 1, ENC },
    { "mtu",       "RTP MTU",                       OFFSET(mtu),       AV_OPT_TYPE_INT,    { .i64 = 1400 },  100, 65500, ENC },
    { NULL }
};

static const AVClass zstr_rtspserver_class = {
    .class_name = "zstr_rtspserver",
    .item_name  = av_default_item_name,
    .option     = zstr_rtspserver_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int rtsp_write_header(AVFormatContext *s)
{
    RTSMuxContext *ctx = s->priv_data;
    const char *mount = ctx->mount ? ctx->mount : "live";

    /* Also accept mount from URL path: rtsp://host:port/<mount> */
    char url_mount[64] = "";
    if (s->url && s->url[0]) {
        const char *p = strstr(s->url, "://");
        p = p ? strchr(p + 3, '/') : s->url;
        if (p) {
            p += strspn(p, "/");
            size_t i = 0;
            while (*p && *p != '/' && *p != '?' && i + 1 < sizeof(url_mount))
                url_mount[i++] = *p++;
            url_mount[i] = '\0';
        }
        /* Optional :port override */
        const char *hp = strstr(s->url, "://");
        hp = hp ? hp + 3 : s->url;
        const char *colon = strrchr(hp, ':');
        const char *slash = strchr(hp, '/');
        if (colon && (!slash || colon < slash)) ctx->port = atoi(colon + 1);
    }
    if (url_mount[0]) mount = url_mount;

    ctx->server = server_create(ctx->port, mount, ctx->force_tcp, ctx->mtu);
    if (!ctx->server) return AVERROR(EIO);
    ctx->port = ctx->server->port; /* reflect ephemeral binding */

    rtsp_session_t *sess = &ctx->server->session;
    for (unsigned i = 0; i < s->nb_streams && i < 2; i++) {
        AVStream *st = s->streams[i];
        AVCodecParameters *cp = st->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && !sess->has_video) {
            if (cp->codec_id == AV_CODEC_ID_H264) sess->video_codec = CODEC_H264;
            else if (cp->codec_id == AV_CODEC_ID_H265) sess->video_codec = CODEC_H265;
            else return AVERROR(EINVAL);
            sess->has_video = 1;
            sess->width = cp->width;
            sess->height = cp->height;
            if (st->avg_frame_rate.den > 0)
                sess->framerate = av_q2d(st->avg_frame_rate);
            if (cp->extradata && cp->extradata_size > 0) {
                sess->extra_data = malloc(cp->extradata_size);
                if (!sess->extra_data) return AVERROR(ENOMEM);
                memcpy(sess->extra_data, cp->extradata, cp->extradata_size);
                sess->extra_size = cp->extradata_size;
            }
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && !sess->has_audio) {
            if (cp->codec_id != AV_CODEC_ID_AAC) return AVERROR(EINVAL);
            sess->has_audio = 1;
            sess->sample_rate = cp->sample_rate;
            sess->channels = cp->ch_layout.nb_channels;
        }
    }
    if (!sess->has_video && !sess->has_audio) return AVERROR(EINVAL);
    return 0;
}

static int rtsp_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    RTSMuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->server || !pkt) return 0;
    if (pkt->stream_index < 0 || pkt->stream_index >= (int)s->nb_streams) return 0;
    AVStream *st = s->streams[pkt->stream_index];
    int is_video = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
    server_push_packet(ctx->server, pkt, st->time_base, is_video);
    return 0;
}

static int rtsp_write_trailer(AVFormatContext *s)
{
    RTSMuxContext *ctx = s->priv_data;
    if (ctx && ctx->server) server_free(&ctx->server);
    return 0;
}

const FFOutputFormat ff_zstr_rtspserver_muxer = {
    .p = {
        .name           = "zstr_rtspserver",
        .long_name      = "zff RTSP Server",
        .extensions     = NULL,
        .audio_codec    = AV_CODEC_ID_AAC,
        .video_codec    = AV_CODEC_ID_H264,
        .subtitle_codec = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
        .priv_class     = &zstr_rtspserver_class,
    },
    .priv_data_size = sizeof(RTSMuxContext),
    .write_header   = rtsp_write_header,
    .write_packet   = rtsp_write_packet,
    .write_trailer  = rtsp_write_trailer,
    .check_bitstream = NULL,
};
