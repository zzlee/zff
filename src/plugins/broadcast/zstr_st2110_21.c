/*=============================================================================
    zstr_st2110_21.c — SMPTE ST 2110-21 Sender Traffic Shaper (Narrow profile)

    NOTE on naming: zstreamer's st2110_21_payloader.c is an H.264/H.265 RTP
    payloader with timestamp pacing bolted on — it does NOT implement
    ST 2110-21. This file implements the real thing: linear (Narrow-type)
    packet emission scheduling for uncompressed (-20) senders.

    Model (ST 2110-21 Narrow, TRO=0):
      - A frame of P packets over frame period T emits packet k at
        t0 + k * (T / P), i.e. evenly spaced, no bursts.
      - If the sender falls behind (missed its slot), the pacer sends
        immediately and rebases (never emits catch-up bursts — bursts are
        exactly what Narrow forbids).
      - Uses CLOCK_MONOTONIC; all times in nanoseconds.

    Usage wraps the -20 payloader output:
      zstr_st2110_21_frame_start(pacer, rtp_ts, nb_packets);
      for each packet: zstr_st2110_21_wait_packet(pacer); sendto(...);
 =============================================================================*/
#include "zff/plugins/zstr_st2110_toolkit.h"
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#define NSEC_PER_SEC 1000000000LL

struct zstr_st2110_21_pacer {
    int width;
    int height;
    int fps_num;
    int fps_den;
    int pacer_type; /* 0 = Narrow (linear); reserved: 1 = Wide */

    int64_t frame_period_ns;
    int64_t packet_interval_ns;
    int packets_this_frame;

    int64_t frame_t0_ns; /* emission start of current frame */
    int packets_sent;
    uint32_t cur_ts;
    int have_frame;

    uint64_t late_packets; /* diagnostics: slots missed */
};

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

zstr_st2110_21_pacer_t *zstr_st2110_21_pacer_create(
    const zstr_st2110_21_config_t *cfg)
{
    zstr_st2110_21_pacer_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->width = (cfg && cfg->width > 0) ? cfg->width : 1920;
    s->height = (cfg && cfg->height > 0) ? cfg->height : 1080;
    s->fps_num = (cfg && cfg->fps_num > 0) ? cfg->fps_num : 60000;
    s->fps_den = (cfg && cfg->fps_den > 0) ? cfg->fps_den : 1001;
    s->pacer_type = (cfg) ? cfg->pacer_type : 0;

    s->frame_period_ns = (int64_t)s->fps_den * NSEC_PER_SEC / s->fps_num;
    if (s->frame_period_ns <= 0) s->frame_period_ns = NSEC_PER_SEC / 60;
    return s;
}

int zstr_st2110_21_frame_start(zstr_st2110_21_pacer_t *s, uint32_t rtp_ts,
                               int nb_packets)
{
    if (!s || nb_packets <= 0) return -1;
    /* New timestamp always starts a new frame; repeated calls with the same
     * timestamp are idempotent (late duplicates from retransmit paths). */
    if (s->have_frame && rtp_ts == s->cur_ts) return 0;
    s->cur_ts = rtp_ts;
    s->packets_this_frame = nb_packets;
    s->packets_sent = 0;
    s->packet_interval_ns = s->frame_period_ns / nb_packets;
    if (s->packet_interval_ns <= 0) s->packet_interval_ns = 1;
    s->frame_t0_ns = mono_ns();
    s->have_frame = 1;
    return 0;
}

int zstr_st2110_21_wait_packet(zstr_st2110_21_pacer_t *s)
{
    if (!s || !s->have_frame) return -1;
    if (s->packets_sent >= s->packets_this_frame) return -1;

    int64_t due = s->frame_t0_ns +
                  (int64_t)s->packets_sent * s->packet_interval_ns;
    int64_t now = mono_ns();
    if (s->packets_sent == 0) {
        /* First packet of the frame goes immediately; rebase the schedule
         * to the actual emission time so frame_start→wait latency never
         * counts as a missed slot. Idling past the whole frame still
         * counts one late slot. */
        if (now - s->frame_t0_ns > s->frame_period_ns)
            s->late_packets++;
        s->frame_t0_ns = now;
        s->packets_sent = 1;
        return 0;
    }
    if (due > now) {
        int64_t wait_ns = due - now;
        struct timespec ts;
        ts.tv_sec = (time_t)(wait_ns / NSEC_PER_SEC);
        ts.tv_nsec = (long)(wait_ns % NSEC_PER_SEC);
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
            /* retry with remaining time */
        }
    } else {
        /* Missed slot: send now, rebase so no catch-up burst follows */
        s->late_packets++;
        s->frame_t0_ns = mono_ns();
        s->packets_sent = 0;
        /* fall through: emit packet 0 of the rebased schedule now */
        s->packets_sent = 1;
        return 0;
    }
    s->packets_sent++;
    return 0;
}

uint64_t zstr_st2110_21_late_count(const zstr_st2110_21_pacer_t *s)
{
    return s ? s->late_packets : 0;
}

int64_t zstr_st2110_21_packet_interval_ns(const zstr_st2110_21_pacer_t *s)
{
    return s ? s->packet_interval_ns : -1;
}

void zstr_st2110_21_pacer_free(zstr_st2110_21_pacer_t **ps)
{
    if (!ps || !*ps) return;
    free(*ps);
    *ps = NULL;
}
