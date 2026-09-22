/*=============================================================================
    zstr_st2110_sdp.c — SMPTE ST 2110 SDP Generate & Parse

    Ported from zstreamer's sdp_muxer.c (st2110 generate mode) and
    sdp_demuxer.c (conservative line parser), reduced to the ST 2110
    subset: raw video (-20) + L16/L24 audio (-30) + ts-refclk/mediaclk.
    The jitter-buffer/reorder/DPLL machinery of sdp_demuxer is NOT
    ported (that belongs to a receiver buffer, not SDP parsing).
=============================================================================*/
#include "zff/plugins/zstr_st2110_sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#include <libavutil/error.h>

/* --- Generate (zstreamer sdp_muxer st2110 mode) --- */

static int append(char **out, size_t *rem, const char *fmt, ...)
{
    if (*rem == 0) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*out, *rem, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= *rem) {
        *rem = 0;
        return -1;
    }
    *out += n;
    *rem -= (size_t)n;
    return 0;
}

int zstr_st2110_sdp_generate(const zstr_st2110_sdp_config_t *cfg, char *out, size_t out_size)
{
    if (!cfg || !out || out_size == 0) return AVERROR(EINVAL);
    if (!cfg->video_enabled && !cfg->audio_enabled) return AVERROR(EINVAL);

    char *p = out;
    size_t rem = out_size;
    uint64_t sid = (uint64_t)time(NULL);
    const char *addr = cfg->address[0] ? cfg->address : "239.0.0.1";
    const char *sname = cfg->session_name[0] ? cfg->session_name : "zff-st2110";

    if (append(&p, &rem, "v=0\r\n") < 0) return AVERROR(ENOMEM);
    if (append(&p, &rem, "o=%s %llu 1 IN IP4 %s\r\n", sname,
               (unsigned long long)sid, addr) < 0) return AVERROR(ENOMEM);
    if (append(&p, &rem, "s=%s\r\n", sname) < 0) return AVERROR(ENOMEM);
    if (append(&p, &rem, "c=IN IP4 %s\r\n", addr) < 0) return AVERROR(ENOMEM);
    if (append(&p, &rem, "t=0 0\r\n") < 0) return AVERROR(ENOMEM);
    if (append(&p, &rem, "a=tool:zff\r\n") < 0) return AVERROR(ENOMEM);
    if (append(&p, &rem, "a=keywait:70\r\n") < 0) return AVERROR(ENOMEM);
    if (cfg->ptp_address[0]) {
        if (append(&p, &rem, "a=ts-refclk:ptp=IEEE1588-2019:%s:%d\r\n",
                   cfg->ptp_address, cfg->ptp_domain) < 0) return AVERROR(ENOMEM);
    }

    if (cfg->video_enabled) {
        int w = cfg->width > 0 ? cfg->width : 1920;
        int h = cfg->height > 0 ? cfg->height : 1080;
        int depth = (cfg->depth == 8 || cfg->depth == 12) ? cfg->depth : 10;
        const char *sampling = cfg->video_sampling[0] ? cfg->video_sampling : "YCbCr-4:2:2";
        if (append(&p, &rem, "m=video %d RTP/AVP %d\r\n", cfg->video_port,
                   cfg->video_pt) < 0) return AVERROR(ENOMEM);
        if (append(&p, &rem, "a=rtpmap:%d raw/90000\r\n", cfg->video_pt) < 0)
            return AVERROR(ENOMEM);
        if (append(&p, &rem, "a=fmtp:%d sampling=%s;width=%d;height=%d;depth=%d\r\n",
                   cfg->video_pt, sampling, w, h, depth) < 0) return AVERROR(ENOMEM);
    }

    if (cfg->audio_enabled) {
        int rate = cfg->sample_rate > 0 ? cfg->sample_rate : 48000;
        int ch = cfg->channels > 0 ? cfg->channels : 2;
        const char *enc = (cfg->audio_depth == 24) ? "L24" : "L16";
        if (append(&p, &rem, "m=audio %d RTP/AVP %d\r\n", cfg->audio_port,
                   cfg->audio_pt) < 0) return AVERROR(ENOMEM);
        if (append(&p, &rem, "a=rtpmap:%d %s/%d/%d\r\n", cfg->audio_pt, enc,
                   rate, ch) < 0) return AVERROR(ENOMEM);
        if (append(&p, &rem, "a=mediaclk:direct=0\r\n") < 0) return AVERROR(ENOMEM);
    }

    return (int)(p - out);
}

/* --- Parse (conservative line parser, zstreamer sdp_demuxer style) --- */

static const char *trim_left(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Extract key=value from a ';'-separated fmtp list into out (0 on found). */
static int fmtp_get_param(const char *fmtp, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = fmtp;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (strncasecmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *v = p + key_len + 1;
            size_t n = strcspn(v, "; \t\r\n");
            if (n >= out_size) n = out_size - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        p += strcspn(p, ";");
        if (*p == ';') p++;
        else break;
    }
    return -1;
}

int zstr_st2110_sdp_parse(const char *sdp, zstr_st2110_sdp_config_t *out_cfg)
{
    if (!sdp || !out_cfg) return AVERROR(EINVAL);

    int cur_media = 0; /* 0 = session, 1 = video, 2 = audio */
    int cur_pt = -1;
    int got_media = 0;

    const char *p = sdp;
    const char *end = sdp + strlen(sdp);
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        size_t len = (size_t)(eol - p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n')) len--;

        char line[512];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        const char *t = trim_left(line);

        if (t[0] == 'm' && t[1] == '=') {
            cur_media = 0;
            cur_pt = -1;
            if (strncasecmp(t + 2, "video", 5) == 0) cur_media = 1;
            else if (strncasecmp(t + 2, "audio", 5) == 0) cur_media = 2;
            if (cur_media) {
                /* m=<media> <port> <proto> <fmt...> */
                const char *rp = t + 2;
                rp += strcspn(rp, " \t");          /* media */
                rp = trim_left(rp);
                int port = atoi(rp);
                rp += strcspn(rp, " \t");          /* port */
                rp = trim_left(rp);
                rp += strcspn(rp, " \t");          /* proto */
                rp = trim_left(rp);
                int pt = atoi(rp);                  /* first fmt */
                if (cur_media == 1 && !out_cfg->video_enabled) {
                    out_cfg->video_enabled = 1;
                    out_cfg->video_port = port;
                    out_cfg->video_pt = pt;
                    out_cfg->depth = 10; /* default per -20 practice */
                    got_media = 1;
                } else if (cur_media == 2 && !out_cfg->audio_enabled) {
                    out_cfg->audio_enabled = 1;
                    out_cfg->audio_port = port;
                    out_cfg->audio_pt = pt;
                    out_cfg->audio_depth = 16;
                    got_media = 1;
                }
                cur_pt = pt;
            }
        } else if (t[0] == 'c' && t[1] == '=' && cur_media == 0) {
            /* c=IN IP4 <addr> (session level overrides default) */
            const char *rp = trim_left(t + 2);
            if (strncasecmp(rp, "IN IP4", 6) == 0) {
                rp = trim_left(rp + 6);
                size_t n = strcspn(rp, " \t\r\n");
                if (n > 0 && n < sizeof(out_cfg->address)) {
                    memcpy(out_cfg->address, rp, n);
                    out_cfg->address[n] = '\0';
                }
            }
        } else if (t[0] == 's' && t[1] == '=' && cur_media == 0 && !out_cfg->session_name[0]) {
            size_t n = strlen(trim_left(t + 2));
            if (n >= sizeof(out_cfg->session_name)) n = sizeof(out_cfg->session_name) - 1;
            memcpy(out_cfg->session_name, trim_left(t + 2), n);
            out_cfg->session_name[n] = '\0';
        } else if (t[0] == 'a' && t[1] == '=') {
            const char *a = t + 2;
            if (strncasecmp(a, "ts-refclk:ptp=IEEE1588-2019:", 28) == 0) {
                const char *v = a + 28;
                const char *colon = strchr(v, ':');
                if (colon) {
                    size_t n = (size_t)(colon - v);
                    if (n >= sizeof(out_cfg->ptp_address)) n = sizeof(out_cfg->ptp_address) - 1;
                    memcpy(out_cfg->ptp_address, v, n);
                    out_cfg->ptp_address[n] = '\0';
                    out_cfg->ptp_domain = atoi(colon + 1);
                }
            } else if (strncasecmp(a, "rtpmap:", 7) == 0 && cur_media && cur_pt >= 0) {
                int pt = atoi(a + 7);
                if (pt == cur_pt) {
                    const char *sp = strchr(a + 7, ' ');
                    if (sp) {
                        sp = trim_left(sp);
                        if (cur_media == 2) {
                            /* L16/48000/2 or L24/... */
                            if (strncasecmp(sp, "L24", 3) == 0) out_cfg->audio_depth = 24;
                            else if (strncasecmp(sp, "L16", 3) == 0) out_cfg->audio_depth = 16;
                            const char *sl = strchr(sp, '/');
                            if (sl) {
                                out_cfg->sample_rate = atoi(sl + 1);
                                const char *sl2 = strchr(sl + 1, '/');
                                if (sl2) out_cfg->channels = atoi(sl2 + 1);
                            }
                        }
                    }
                }
            } else if (strncasecmp(a, "fmtp:", 5) == 0 && cur_media == 1 && cur_pt >= 0) {
                int pt = atoi(a + 5);
                if (pt == cur_pt) {
                    const char *sp = strchr(a + 5, ' ');
                    const char *fmtp = sp ? sp + 1 : "";
                    char val[32];
                    if (fmtp_get_param(fmtp, "sampling", val, sizeof(val)) == 0) {
                        strncpy(out_cfg->video_sampling, val, sizeof(out_cfg->video_sampling) - 1);
                    }
                    if (fmtp_get_param(fmtp, "width", val, sizeof(val)) == 0)
                        out_cfg->width = atoi(val);
                    if (fmtp_get_param(fmtp, "height", val, sizeof(val)) == 0)
                        out_cfg->height = atoi(val);
                    if (fmtp_get_param(fmtp, "depth", val, sizeof(val)) == 0)
                        out_cfg->depth = atoi(val);
                }
            }
        }

        p = (eol < end) ? eol + 1 : end;
    }

    return got_media ? 0 : AVERROR_INVALIDDATA;
}
