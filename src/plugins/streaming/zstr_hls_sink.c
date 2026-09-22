/*=============================================================================
    zstr_hls_sink.c — HLS segmenter (FFOutputFormat driving native hls muxer)

    Ported from zstreamer hls_sink.c, minus the framework (pads/caps):
    - Streams are copied from the outer context (video H264/HEVC, audio
      AAC/Opus/PCM); video extradata (avcC) is REQUIRED at write_header
      time, exactly like the native hls muxer expects from encoders.
    - H.264/H.265 packets in Annex-B form are converted to AVCC per
      packet (hls_annexb_to_avcc); already-length-prefixed packets pass
      through untouched.
    - HLS options: hls_time, hls_list_size, mpegts/fmp4 segments.
    - Serving the output directory over HTTP is left to any static file
      server (zstreamer's micro http_server.c is deliberately not ported).
 =============================================================================*/
#define _GNU_SOURCE

#include "zff/plugins/zstr_hls_sink.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

typedef struct HLSMuxContext {
    const AVClass *av_class;
    char *location;
    int hls_time;
    int hls_list_size;
    char *segment_type;
    AVFormatContext *hls_ctx;
} HLSMuxContext;

#define OFFSET(x) offsetof(HLSMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_hls_sink_options[] = {
    { "location",      "Output playlist path (e.g. /tmp/hls/out.m3u8)", OFFSET(location),      AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, ENC },
    { "hls_time",      "Segment duration in seconds",                   OFFSET(hls_time),      AV_OPT_TYPE_INT,    { .i64 = 2 },   1, 3600, ENC },
    { "hls_list_size", "Playlist entries kept",                         OFFSET(hls_list_size), AV_OPT_TYPE_INT,    { .i64 = 5 },   0, 1000, ENC },
    { "segment_type",  "Segment container (mpegts, fmp4)",              OFFSET(segment_type),  AV_OPT_TYPE_STRING, { .str = "mpegts" }, 0, 0, ENC },
    { NULL }
};

static const AVClass zstr_hls_sink_class = {
    .class_name = "zstr_hls_sink",
    .item_name  = av_default_item_name,
    .option     = zstr_hls_sink_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* --- Annex-B helpers (from zstreamer hls_sink.c) --- */

static int find_start_code(const uint8_t *data, int size, int pos, int *code_len)
{
    for (int i = pos; i + 2 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                *code_len = 3;
                return i;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                *code_len = 4;
                return i;
            }
        }
    }
    return -1;
}

/* Convert Annex-B to AVCC length-prefixed form. Returns malloc'd buffer
 * (caller frees) with *out_size set, or NULL when no start code found
 * (caller then passes the packet through untouched). */
static uint8_t *annexb_to_avcc(const uint8_t *data, int size, int *out_size)
{
    *out_size = 0;
    int code_size = 0;
    int pos = find_start_code(data, size, 0, &code_size);
    if (pos < 0) return NULL;

    uint8_t *out = malloc((size_t)size + (size_t)size / 2 + 4);
    if (!out) return NULL;
    int out_pos = 0;

    while (pos >= 0 && pos < size) {
        int nal_start = pos + code_size;
        int next_code_size = 0;
        int next = find_start_code(data, size, nal_start, &next_code_size);
        int nal_end = next >= 0 ? next : size;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        int nal_size = nal_end - nal_start;
        if (nal_size > 0) {
            out[out_pos++] = (uint8_t)(nal_size >> 24);
            out[out_pos++] = (uint8_t)(nal_size >> 16);
            out[out_pos++] = (uint8_t)(nal_size >> 8);
            out[out_pos++] = (uint8_t)(nal_size);
            memcpy(out + out_pos, data + nal_start, nal_size);
            out_pos += nal_size;
        }
        if (next < 0) break;
        code_size = next_code_size;
        pos = next;
    }

    *out_size = out_pos;
    return out;
}

/* --- Muxer --- */

static int hls_write_header(AVFormatContext *s)
{
    HLSMuxContext *ctx = s->priv_data;

    const char *location = NULL;
    if (s->url && s->url[0]) {
        location = s->url;
    } else if (ctx->location && ctx->location[0]) {
        location = ctx->location;
    }
    if (!location) {
        av_log(s, AV_LOG_ERROR, "zstr_hls_sink requires location\n");
        return AVERROR(EINVAL);
    }

    if (s->nb_streams == 0) return AVERROR(EINVAL);

    AVFormatContext *hc = NULL;
    if (avformat_alloc_output_context2(&hc, NULL, "hls", location) < 0 || !hc)
        return AVERROR(EIO);
    ctx->hls_ctx = hc;

    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *in_st = s->streams[i];
        AVCodecParameters *cp = in_st->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (cp->codec_id != AV_CODEC_ID_H264 && cp->codec_id != AV_CODEC_ID_HEVC)
                return AVERROR(EINVAL);
            if (!cp->extradata || cp->extradata_size <= 0) {
                av_log(s, AV_LOG_ERROR,
                       "zstr_hls_sink: video stream %u needs avcC extradata\n", i);
                return AVERROR(EINVAL);
            }
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (cp->codec_id != AV_CODEC_ID_AAC && cp->codec_id != AV_CODEC_ID_OPUS &&
                cp->codec_id != AV_CODEC_ID_PCM_S16LE)
                return AVERROR(EINVAL);
        } else {
            return AVERROR(EINVAL);
        }

        AVStream *st = avformat_new_stream(hc, NULL);
        if (!st) return AVERROR(ENOMEM);
        if (avcodec_parameters_copy(st->codecpar, cp) < 0) return AVERROR(ENOMEM);
        st->time_base = in_st->time_base;
    }

    AVDictionary *opts = NULL;
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", ctx->hls_time > 0 ? ctx->hls_time : 2);
    av_dict_set(&opts, "hls_time", tmp, 0);
    snprintf(tmp, sizeof(tmp), "%d", ctx->hls_list_size);
    av_dict_set(&opts, "hls_list_size", tmp, 0);
    if (ctx->segment_type && strcasecmp(ctx->segment_type, "fmp4") == 0)
        av_dict_set(&opts, "hls_segment_type", "fmp4", 0);
    else
        av_dict_set(&opts, "hls_segment_type", "mpegts", 0);
    av_dict_set(&opts, "hls_flags", "independent_segments", 0);

    int ret = avformat_write_header(hc, &opts);
    av_dict_free(&opts);
    return ret;
}

static int hls_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    HLSMuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->hls_ctx || !pkt) return 0;
    if (pkt->stream_index < 0 || pkt->stream_index >= (int)s->nb_streams) return 0;

    AVStream *in_st = s->streams[pkt->stream_index];
    AVPacket *out = av_packet_alloc();
    if (!out) return AVERROR(ENOMEM);
    if (av_packet_ref(out, pkt) < 0) {
        av_packet_free(&out);
        return AVERROR(ENOMEM);
    }

    /* Annex-B video -> AVCC (detect by start code) */
    if (in_st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && pkt->size >= 3 &&
        pkt->data[0] == 0 && pkt->data[1] == 0 &&
        (pkt->data[2] == 1 ||
         (pkt->size >= 4 && pkt->data[2] == 0 && pkt->data[3] == 1))) {
        int avcc_size = 0;
        uint8_t *avcc = annexb_to_avcc(pkt->data, pkt->size, &avcc_size);
        if (avcc && avcc_size > 0) {
            av_packet_unref(out);
            if (av_new_packet(out, avcc_size) < 0) {
                free(avcc);
                av_packet_free(&out);
                return AVERROR(ENOMEM);
            }
            memcpy(out->data, avcc, avcc_size);
            free(avcc);
            out->pts = pkt->pts;
            out->dts = pkt->dts;
            out->duration = pkt->duration;
            out->stream_index = pkt->stream_index;
            out->flags = pkt->flags;
            out->time_base = pkt->time_base;
        } else {
            free(avcc);
        }
    }

    int ret = av_write_frame(ctx->hls_ctx, out);
    av_packet_free(&out);
    return ret;
}

static int hls_write_trailer(AVFormatContext *s)
{
    HLSMuxContext *ctx = s->priv_data;
    if (ctx && ctx->hls_ctx) {
        av_write_trailer(ctx->hls_ctx);
        avformat_free_context(ctx->hls_ctx);
        ctx->hls_ctx = NULL;
    }
    return 0;
}

const FFOutputFormat ff_zstr_hls_sink_muxer = {
    .p = {
        .name           = "zstr_hls_sink",
        .long_name      = "zff HLS Segmenter",
        .extensions     = "m3u8",
        .audio_codec    = AV_CODEC_ID_AAC,
        .video_codec    = AV_CODEC_ID_H264,
        .subtitle_codec = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
        .priv_class     = &zstr_hls_sink_class,
    },
    .priv_data_size = sizeof(HLSMuxContext),
    .write_header   = hls_write_header,
    .write_packet   = hls_write_packet,
    .write_trailer  = hls_write_trailer,
    .check_bitstream = NULL,
};
