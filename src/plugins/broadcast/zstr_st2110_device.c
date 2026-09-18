/*=============================================================================
    zstr_st2110_device.c — FFmpeg Device Integration for SMPTE ST 2110
=============================================================================*/
#define _GNU_SOURCE

#include "zff/plugins/zstr_st2110.h"
#include "zff/plugins/zstr_net.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavutil/opt.h>
#include <libavutil/error.h>

typedef struct ST2110MuxContext {
    const AVClass *av_class;
    char *host;
    int port;
    int payload_type;
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

    zstr_net_config_t net_cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = ctx->host ? ctx->host : "127.0.0.1",
        .port = (uint16_t)(ctx->port ? ctx->port : 20000),
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

    return 0;
}

static int st2110_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    ST2110MuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->net_sink || !pkt) return 0;

    /* Write directly over network sink */
    return zstr_net_sink_write_packet(ctx->net_sink, pkt);
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
    zstr_net_source_t *net_src;
} ST2110DemuxContext;

#define OFFSET_D(x) offsetof(ST2110DemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_st2110_demux_options[] = {
    { "host", "Bind IP address", OFFSET_D(host), AV_OPT_TYPE_STRING, { .str = "0.0.0.0" }, 0, 0, DEC },
    { "port", "Bind UDP port",   OFFSET_D(port), AV_OPT_TYPE_INT,    { .i64 = 20000 },     0, 65535, DEC },
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
    zstr_net_config_t cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = ctx->host ? ctx->host : "0.0.0.0",
        .port = (uint16_t)(ctx->port ? ctx->port : 20000),
        .buffer_size = 65536,
        .timeout_ms = 1000
    };
    ctx->net_src = zstr_net_source_create(&cfg);
    if (!ctx->net_src) return AVERROR(EIO);

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
