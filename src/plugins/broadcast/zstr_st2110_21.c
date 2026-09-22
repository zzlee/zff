/*=============================================================================
    zstr_st2110_21.c — SMPTE ST 2110-21 Sender Traffic Shaper + Receiver Monitor

    NOTE on naming: zstreamer's st2110_21_payloader.c is an H.264/H.265 RTP
    payloader with timestamp pacing bolted on — it does NOT implement
    ST 2110-21. This file implements the real thing:

    Pacer (sender side, TRO=0):
    - Narrow (type 0): a frame of P packets over frame period T emits
      packet k at t0 + k * (T / P), evenly spaced, no bursts. On a missed
      slot the pacer sends immediately and REBASES (never catch-up bursts).
    - Wide (type 1): same linear schedule, but a missed slot sends
      immediately WITHOUT rebasing, so the sender catches up in a burst
      (up to cmax consecutive immediate sends, then forced rebase as a
      safety). Receivers absorb bursts via their VRX.
    - Uses CLOCK_MONOTONIC; all times in nanoseconds.

    Monitor (receiver side): leaky-bucket VRX measurement (drains at the
    media rate from first arrival), burst/CMAX observation, and lateness
    vs the ideal schedule. Thresholds are zff measurement defaults, NOT
    SMPTE certification values (those live in the standard's tables).

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
    int pacer_type; /* 0 = Narrow (linear); 1 = Wide (burst-tolerant) */
    int cmax;         /* Wide: max consecutive immediate sends before rebase */

    int64_t frame_period_ns;
    int64_t packet_interval_ns;
    int packets_this_frame;

    int64_t frame_t0_ns; /* emission start of current frame */
    int packets_sent;
    int burst_run;       /* Wide: consecutive immediate sends */
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
    s->cmax = (cfg && cfg->cmax > 0) ? cfg->cmax : 16;

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
    s->burst_run = 0;
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
        s->burst_run = 0;
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
        s->burst_run = 0;
    } else if (s->pacer_type == 1 && s->burst_run < s->cmax) {
        /* Wide: send immediately WITHOUT rebasing (catch-up burst for
         * the receiver's VRX); the original schedule is preserved. */
        s->late_packets++;
        s->burst_run++;
    } else {
        /* Narrow: send now and rebase so no catch-up burst follows.
         * Wide past cmax: forced rebase as a drift safety. */
        s->late_packets++;
        s->frame_t0_ns = mono_ns();
        s->packets_sent = 0;
        /* fall through: emit packet 0 of the rebased schedule now */
        s->packets_sent = 1;
        s->burst_run = 0;
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

/* --- Receiver monitor --- */

struct zstr_st2110_21_monitor {
    int packets_per_frame;
    int64_t frame_period_ns;
    int64_t interval_ns;
    int vrx_full;
    int cmax_limit;
    int64_t late_limit_ns;

    int have_stream;
    uint16_t first_seq;
    uint32_t seq0_ext;
    uint32_t last_ext;
    int64_t t0_ns;

    double vrx_occ;
    double max_vrx_occ;
    uint64_t overflows;
    int64_t last_arrival_ns;

    int burst_run;
    int max_burst;
    int64_t max_late_ns;
    uint64_t late_count;
    uint64_t total;
};

zstr_st2110_21_monitor_t *zstr_st2110_21_monitor_create(
    const zstr_st2110_21_monitor_config_t *cfg)
{
    zstr_st2110_21_monitor_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    int ppf = (cfg && cfg->packets_per_frame > 0) ? cfg->packets_per_frame : 100;
    int fn = (cfg && cfg->fps_num > 0) ? cfg->fps_num : 60000;
    int fd = (cfg && cfg->fps_den > 0) ? cfg->fps_den : 1001;
    s->packets_per_frame = ppf;
    s->frame_period_ns = (int64_t)fd * NSEC_PER_SEC / fn;
    if (s->frame_period_ns <= 0) s->frame_period_ns = NSEC_PER_SEC / 60;
    s->interval_ns = s->frame_period_ns / ppf;
    if (s->interval_ns <= 0) s->interval_ns = 1;
    s->vrx_full = (cfg && cfg->vrx_full > 0) ? cfg->vrx_full : 16;
    s->cmax_limit = (cfg && cfg->cmax_limit > 0) ? cfg->cmax_limit : 8;
    s->late_limit_ns = (cfg && cfg->late_limit_ns > 0) ? cfg->late_limit_ns
                                                       : 2 * s->interval_ns;
    return s;
}

static uint32_t mon_unwrap(uint16_t seq, uint32_t last_ext, int have_last)
{
    if (!have_last) return seq;
    uint32_t base = last_ext & 0xFFFF0000u;
    uint32_t cand = base | seq;
    if (cand + 0x8000u < last_ext) cand += 0x10000u;
    else if (cand > last_ext + 0x8000u && cand >= 0x10000u) cand -= 0x10000u;
    return cand;
}

int zstr_st2110_21_monitor_packet(zstr_st2110_21_monitor_t *s, uint16_t seq,
                                  int64_t arrival_ns)
{
    if (!s) return -1;
    s->total++;

    if (!s->have_stream) {
        s->have_stream = 1;
        s->first_seq = seq;
        s->seq0_ext = seq;
        s->last_ext = seq;
        s->t0_ns = arrival_ns;
        s->vrx_occ = 1.0;
        s->max_vrx_occ = 1.0;
        s->last_arrival_ns = arrival_ns;
        s->burst_run = 1;
        s->max_burst = 1;
        return 0;
    }

    uint32_t ext = mon_unwrap(seq, s->last_ext, 1);
    if (ext <= s->last_ext) {
        /* Duplicate or reorder: counted for burst, ignored for schedule */
        if (arrival_ns - s->last_arrival_ns < s->interval_ns) {
            s->burst_run++;
            if (s->burst_run > s->max_burst) s->max_burst = s->burst_run;
        } else {
            s->burst_run = 1;
        }
        s->last_arrival_ns = arrival_ns;
        return 0;
    }
    s->last_ext = ext;

    /* Leaky-bucket VRX: drain at the media rate since last arrival */
    double elapsed = (double)(arrival_ns - s->last_arrival_ns);
    if (elapsed < 0) elapsed = 0;
    s->vrx_occ -= elapsed / (double)s->interval_ns;
    if (s->vrx_occ < 0) s->vrx_occ = 0;
    s->vrx_occ += 1.0;
    if (s->vrx_occ > s->max_vrx_occ) s->max_vrx_occ = s->vrx_occ;
    if (s->vrx_occ > (double)s->vrx_full) s->overflows++;

    /* Burst run: arrivals closer than one interval */
    if (arrival_ns - s->last_arrival_ns < s->interval_ns) {
        s->burst_run++;
        if (s->burst_run > s->max_burst) s->max_burst = s->burst_run;
    } else {
        s->burst_run = 1;
    }
    s->last_arrival_ns = arrival_ns;

    /* Lateness vs the ideal linear schedule from first arrival */
    uint32_t k = ext - s->seq0_ext;
    int64_t due = s->t0_ns + (int64_t)k * s->interval_ns;
    int64_t late = arrival_ns - due;
    if (late > 0) {
        s->late_count++;
        if (late > s->max_late_ns) s->max_late_ns = late;
    }
    return 0;
}

int zstr_st2110_21_monitor_check(const zstr_st2110_21_monitor_t *s,
                                 zstr_st2110_21_report_t *report)
{
    if (!s || !report) return -1;
    report->max_vrx_occ = s->max_vrx_occ;
    report->overflows = s->overflows;
    report->max_burst = s->max_burst;
    report->max_late_ns = s->max_late_ns;
    report->late_count = s->late_count;
    report->total = s->total;
    report->compliant = (s->overflows == 0 &&
                         s->max_burst <= s->cmax_limit &&
                         s->max_late_ns <= s->late_limit_ns);
    return 0;
}

void zstr_st2110_21_monitor_free(zstr_st2110_21_monitor_t **ps)
{
    if (!ps || !*ps) return;
    free(*ps);
    *ps = NULL;
}
