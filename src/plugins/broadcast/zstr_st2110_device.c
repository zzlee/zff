/*=============================================================================
    zstr_st2110_device.c — FFmpeg Device Integration for SMPTE ST 2110
=============================================================================*/
#define _GNU_SOURCE

#include "zff/plugins/zstr_st2110.h"
#include "zff/plugins/zstr_st2110_toolkit.h"
#include "zff/plugins/zstr_st2110_sdp.h"
#include "zff/plugins/zstr_net.h"
#include "../streaming/zstr_net_internal.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <libavutil/opt.h>
#include <libavutil/error.h>

typedef struct ST2110MuxContext {
    const AVClass *av_class;
    char *host;
    int port;
    int payload_type;
    char *sdp_file;
    zstr_net_sink_t *net_sink;
    zstr_st2110_20_payloader_t *pay20;
    zstr_st2110_30_payloader_t *pay30;
    bool is_video;
} ST2110MuxContext;

#define OFFSET_M(x) offsetof(ST2110MuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_st2110_mux_options[] = {
    { "host", "Destination IP address", OFFSET_M(host), AV_OPT_TYPE_STRING, { .str = "127.0.0.1" }, 0, 0, ENC },
    { "port", "Destination UDP port",    OFFSET_M(port), AV_OPT_TYPE_INT,    { .i64 = 20000 },       0, 65535, ENC },
    { "pt",   "RTP Payload Type",        OFFSET_M(payload_type), AV_OPT_TYPE_INT, { .i64 = 96 },     0, 127, ENC },
    { "sdp_file", "Write ST 2110 SDP to this path at write_header", OFFSET_M(sdp_file), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { NULL }
};

static const AVClass zstr_st2110_mux_class = {
    .class_name = "zstr_st2110_mux",
    .item_name  = av_default_item_name,
    .option     = zstr_st2110_mux_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int st2110_write_header(AVFormatContext *s)
{
    ST2110MuxContext *ctx = s->priv_data;

    char host_buf[128] = {0};
    const char *dest_host = ctx->host ? ctx->host : "127.0.0.1";
    uint16_t dest_port = (uint16_t)(ctx->port ? ctx->port : 20000);

    if (s->url && s->url[0]) {
        const char *proto_sep = strstr(s->url, "://");
        const char *hp = proto_sep ? (proto_sep + 3) : s->url;
        const char *colon = strrchr(hp, ':');
        if (colon) {
            size_t hlen = colon - hp;
            if (hlen > 0 && hlen < sizeof(host_buf)) {
                strncpy(host_buf, hp, hlen);
                dest_host = host_buf;
            }
            dest_port = (uint16_t)atoi(colon + 1);
        }
    }

    zstr_net_config_t net_cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = dest_host,
        .port = dest_port,
        .buffer_size = 65536,
        .timeout_ms = 1000
    };
    ctx->net_sink = zstr_net_sink_create(&net_cfg);
    if (!ctx->net_sink) return AVERROR(EIO);

    ctx->is_video = true;
    if (s->nb_streams > 0 && s->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        ctx->is_video = false;
        zstr_st2110_30_config_t a_cfg = {
            .channels = s->streams[0]->codecpar->ch_layout.nb_channels ? s->streams[0]->codecpar->ch_layout.nb_channels : 2,
            .sample_rate = s->streams[0]->codecpar->sample_rate ? s->streams[0]->codecpar->sample_rate : 48000,
            .bit_depth = 16,
            .payload_type = (uint8_t)(ctx->payload_type ? ctx->payload_type : 97)
        };
        ctx->pay30 = zstr_st2110_30_payloader_create(&a_cfg);
    } else {
        zstr_st2110_20_config_t v_cfg = {
            .width = (s->nb_streams > 0 && s->streams[0]->codecpar->width > 0) ? s->streams[0]->codecpar->width : 1920,
            .height = (s->nb_streams > 0 && s->streams[0]->codecpar->height > 0) ? s->streams[0]->codecpar->height : 1080,
            .fmt = AV_PIX_FMT_UYVY422,
            .payload_type = (uint8_t)(ctx->payload_type ? ctx->payload_type : 96)
        };
        ctx->pay20 = zstr_st2110_20_payloader_create(&v_cfg);
    }

    /* Emit ST 2110 SDP describing this sender (interop handoff) */
    if (ctx->sdp_file && ctx->sdp_file[0]) {
        zstr_st2110_sdp_config_t sdp_cfg;
        memset(&sdp_cfg, 0, sizeof(sdp_cfg));
        snprintf(sdp_cfg.address, sizeof(sdp_cfg.address), "%s", dest_host);
        snprintf(sdp_cfg.session_name, sizeof(sdp_cfg.session_name), "zff-st2110");
        if (s->nb_streams > 0) {
            AVCodecParameters *cp = s->streams[0]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
                sdp_cfg.audio_enabled = 1;
                sdp_cfg.audio_port = dest_port;
                sdp_cfg.audio_pt = ctx->payload_type ? ctx->payload_type : 97;
                sdp_cfg.sample_rate = cp->sample_rate > 0 ? cp->sample_rate : 48000;
                sdp_cfg.channels = cp->ch_layout.nb_channels > 0 ?
                                   cp->ch_layout.nb_channels : 2;
                sdp_cfg.audio_depth = (cp->codec_id == AV_CODEC_ID_PCM_S24BE) ? 24 : 16;
            } else {
                sdp_cfg.video_enabled = 1;
                sdp_cfg.video_port = dest_port;
                sdp_cfg.video_pt = ctx->payload_type ? ctx->payload_type : 96;
                sdp_cfg.width = cp->width > 0 ? cp->width : 1920;
                sdp_cfg.height = cp->height > 0 ? cp->height : 1080;
                sdp_cfg.depth = 10;
                snprintf(sdp_cfg.video_sampling, sizeof(sdp_cfg.video_sampling),
                         "YCbCr-4:2:2");
                if (cp->format == AV_PIX_FMT_RGB24) {
                    snprintf(sdp_cfg.video_sampling, sizeof(sdp_cfg.video_sampling), "RGB");
                    sdp_cfg.depth = 8;
                } else if (cp->format == AV_PIX_FMT_UYVY422) {
                    sdp_cfg.depth = 8;
                }
            }
        }
        char sdp_text[4096];
        int n = zstr_st2110_sdp_generate(&sdp_cfg, sdp_text, sizeof(sdp_text));
        if (n > 0) {
            FILE *f = fopen(ctx->sdp_file, "w");
            if (f) {
                fwrite(sdp_text, 1, (size_t)n, f);
                fclose(f);
            } else {
                av_log(s, AV_LOG_WARNING, "zstr_st2110_mux: cannot write sdp_file %s\n",
                       ctx->sdp_file);
            }
        }
    }

    return 0;
}

static int st2110_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    ST2110MuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->net_sink || !pkt) return 0;

    /* Write directly over network sink */
    int ret = zstr_net_sink_write_packet(ctx->net_sink, pkt);
    return ret >= 0 ? 0 : ret;
}

static int st2110_write_trailer(AVFormatContext *s)
{
    ST2110MuxContext *ctx = s->priv_data;
    if (ctx) {
        if (ctx->pay20) zstr_st2110_20_payloader_free(&ctx->pay20);
        if (ctx->pay30) zstr_st2110_30_payloader_free(&ctx->pay30);
        if (ctx->net_sink) zstr_net_sink_close(&ctx->net_sink);
    }
    return 0;
}

const FFOutputFormat ff_zstr_st2110_muxer = {
    .p = {
        .name           = "zstr_st2110_mux",
        .long_name      = "zff SMPTE ST 2110 Muxer Outdev",
        .extensions     = NULL,
        .audio_codec    = AV_CODEC_ID_PCM_S16BE,
        .video_codec    = AV_CODEC_ID_RAWVIDEO,
        .subtitle_codec = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
        .priv_class     = &zstr_st2110_mux_class,
    },
    .priv_data_size = sizeof(ST2110MuxContext),
    .write_header   = st2110_write_header,
    .write_packet   = st2110_write_packet,
    .write_trailer  = st2110_write_trailer,
    .check_bitstream = NULL,
};

/* ---------------------------------------------------------------------------
 * Demuxer
 * --------------------------------------------------------------------------- */
typedef struct ST2110DemuxContext {
    const AVClass *av_class;
    char *host;
    int port;
    char *sdp_file;
    zstr_net_source_t *net_src;
} ST2110DemuxContext;

#define OFFSET_D(x) offsetof(ST2110DemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_st2110_demux_options[] = {
    { "host", "Bind IP address", OFFSET_D(host), AV_OPT_TYPE_STRING, { .str = "0.0.0.0" }, 0, 0, DEC },
    { "port", "Bind UDP port",   OFFSET_D(port), AV_OPT_TYPE_INT,    { .i64 = 20000 },     0, 65535, DEC },
    { "sdp_file", "Read ST 2110 SDP from this path to build streams (port/address)", OFFSET_D(sdp_file), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, DEC },
    { NULL }
};

static const AVClass zstr_st2110_demux_class = {
    .class_name = "zstr_st2110_demux",
    .item_name  = av_default_item_name,
    .option     = zstr_st2110_demux_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int st2110_read_header(AVFormatContext *s)
{
    ST2110DemuxContext *ctx = s->priv_data;

    char host_buf[128] = {0};
    const char *bind_host = ctx->host ? ctx->host : "0.0.0.0";
    uint16_t bind_port = (uint16_t)(ctx->port ? ctx->port : 20000);

    if (s->url && s->url[0]) {
        const char *proto_sep = strstr(s->url, "://");
        const char *hp = proto_sep ? (proto_sep + 3) : s->url;
        const char *colon = strrchr(hp, ':');
        if (colon) {
            size_t hlen = colon - hp;
            if (hlen > 0 && hlen < sizeof(host_buf)) {
                strncpy(host_buf, hp, hlen);
                bind_host = host_buf;
            }
            bind_port = (uint16_t)atoi(colon + 1);
        }
    }

    zstr_net_config_t cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = bind_host,
        .port = bind_port,
        .buffer_size = 65536,
        .timeout_ms = 1000
    };

    /* SDP-driven stream setup: parse the session file for ports/address
     * and stream geometry. The single-socket device binds the video port
     * (or the audio port when video is absent). */
    zstr_st2110_sdp_config_t sdp_cfg;
    bool have_sdp = false;
    memset(&sdp_cfg, 0, sizeof(sdp_cfg));
    if (ctx->sdp_file && ctx->sdp_file[0]) {
        FILE *f = fopen(ctx->sdp_file, "r");
        if (f) {
            char sdp_text[8192];
            size_t n = fread(sdp_text, 1, sizeof(sdp_text) - 1, f);
            fclose(f);
            sdp_text[n] = '\0';
            if (n > 0 && zstr_st2110_sdp_parse(sdp_text, &sdp_cfg) == 0) {
                have_sdp = true;
                if (sdp_cfg.video_enabled) bind_port = (uint16_t)sdp_cfg.video_port;
                else if (sdp_cfg.audio_enabled) bind_port = (uint16_t)sdp_cfg.audio_port;
                cfg.port = bind_port;
                /* SDP multicast address: let net_source join the group */
                if (sdp_cfg.address[0]) {
                    snprintf(host_buf, sizeof(host_buf), "%s", sdp_cfg.address);
                    bind_host = host_buf;
                    cfg.host = bind_host;
                }
            } else {
                av_log(s, AV_LOG_WARNING,
                       "zstr_st2110_demux: cannot parse sdp_file %s, using host/port\n",
                       ctx->sdp_file);
            }
        } else {
            av_log(s, AV_LOG_WARNING,
                   "zstr_st2110_demux: cannot open sdp_file %s, using host/port\n",
                   ctx->sdp_file);
        }
    }

    ctx->net_src = zstr_net_source_create(&cfg);
    if (!ctx->net_src) return AVERROR(EIO);

    if (have_sdp) {
        if (sdp_cfg.video_enabled) {
            AVStream *st = avformat_new_stream(s, NULL);
            if (!st) return AVERROR(ENOMEM);
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
            st->codecpar->width = sdp_cfg.width > 0 ? sdp_cfg.width : 1920;
            st->codecpar->height = sdp_cfg.height > 0 ? sdp_cfg.height : 1080;
            /* sampling -> pixel format (conservative subset) */
            if (strcasecmp(sdp_cfg.video_sampling, "RGB") == 0)
                st->codecpar->format = AV_PIX_FMT_RGB24;
            else
                st->codecpar->format = AV_PIX_FMT_UYVY422; /* YCbCr-4:2:2 default */
            st->time_base = (AVRational){ 1, 90000 };
        }
        if (sdp_cfg.audio_enabled) {
            AVStream *st = avformat_new_stream(s, NULL);
            if (!st) return AVERROR(ENOMEM);
            st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            st->codecpar->codec_id = (sdp_cfg.audio_depth == 24) ?
                                     AV_CODEC_ID_PCM_S24BE : AV_CODEC_ID_PCM_S16BE;
            st->codecpar->sample_rate = sdp_cfg.sample_rate > 0 ?
                                        sdp_cfg.sample_rate : 48000;
            av_channel_layout_default(&st->codecpar->ch_layout,
                                      sdp_cfg.channels > 0 ? sdp_cfg.channels : 2);
            st->time_base = (AVRational){ 1, st->codecpar->sample_rate };
        }
        return 0;
    }

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->time_base = (AVRational){ 1, 90000 };
    return 0;
}

static int st2110_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    ST2110DemuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->net_src) return AVERROR(EINVAL);
    return zstr_net_source_read_packet(ctx->net_src, pkt);
}

static int st2110_read_close(AVFormatContext *s)
{
    ST2110DemuxContext *ctx = s->priv_data;
    if (ctx && ctx->net_src) {
        zstr_net_source_close(&ctx->net_src);
    }
    return 0;
}

const AVInputFormat ff_zstr_st2110_demuxer = {
    .name           = "zstr_st2110_demux",
    .long_name      = "zff SMPTE ST 2110 Demuxer Indev",
    .flags          = AVFMT_NOFILE,
    .priv_class     = &zstr_st2110_demux_class,
    .priv_data_size = sizeof(ST2110DemuxContext),
    .read_header    = st2110_read_header,
    .read_packet    = st2110_read_packet,
    .read_close     = st2110_read_close,
};
