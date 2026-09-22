/*=============================================================================
    zstr_st2022_7_mux.c — SMPTE ST 2022-7 Seamless Redundancy Sender (dual-send)

    Companion to zstr_st2022_7_demux_process: clones each outgoing RTP
    packet for the primary (A) and secondary (B) paths. Per ST 2022-7 the
    two copies are bit-identical (same SSRC/sequence/timestamps); the
    receiver's demux drops the late duplicate.
=============================================================================*/
#include "zff/plugins/zstr_st2110_toolkit.h"

#include <stdlib.h>

#include <libavutil/error.h>

struct zstr_st2022_7_mux {
    uint64_t packets_duplicated;
};

zstr_st2022_7_mux_t *zstr_st2022_7_mux_create(void)
{
    zstr_st2022_7_mux_t *s = calloc(1, sizeof(*s));
    return s;
}

int zstr_st2022_7_mux_process(zstr_st2022_7_mux_t *s,
                              const AVPacket *in_pkt,
                              AVPacket **out_a,
                              AVPacket **out_b)
{
    if (!s || !in_pkt || !out_a || !out_b) return AVERROR(EINVAL);
    *out_a = NULL;
    *out_b = NULL;

    AVPacket *a = av_packet_alloc();
    AVPacket *b = av_packet_alloc();
    if (!a || !b) {
        av_packet_free(&a);
        av_packet_free(&b);
        return AVERROR(ENOMEM);
    }
    if (av_packet_ref(a, in_pkt) < 0 || av_packet_ref(b, in_pkt) < 0) {
        av_packet_free(&a);
        av_packet_free(&b);
        return AVERROR(ENOMEM);
    }

    s->packets_duplicated++;
    *out_a = a;
    *out_b = b;
    return 0;
}

uint64_t zstr_st2022_7_mux_count(const zstr_st2022_7_mux_t *s)
{
    return s ? s->packets_duplicated : 0;
}

void zstr_st2022_7_mux_free(zstr_st2022_7_mux_t **ps)
{
    if (!ps || !*ps) return;
    free(*ps);
    *ps = NULL;
}
