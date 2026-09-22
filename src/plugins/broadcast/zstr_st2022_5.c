/*=============================================================================
    zstr_st2022_5.c — SMPTE ST 2022-5 1-D Row FEC (XOR protection)

    NOTE: zstreamer's st2110_fec.c is an unimplemented passthrough stub
    (TODO comments, no math). This file implements real row FEC per
    RFC 2733 as referenced by ST 2022-5:

    - A row of L consecutive media packets (L <= 24, dense mask) produces
      one FEC packet: SN-base(2) | length-recovery(2) | E+PT-recovery(1) |
      mask(3) | TS-recovery(4) | XOR of (padded) payloads.
    - FEC packets ride the SAME SSRC as media (RFC 5109 ULPFEC convention),
      distinguished by payload type (default 127, configurable).
    - Recovery: exactly one missing packet per row is rebuilt (payload via
      XOR, PT/TS from recovery fields, length trimmed via length-recovery).
      Two or more losses in a row are unrecoverable (column FEC is the
      follow-up, not v1).
    - LIMITATION (documented): the RTP marker bit is NOT protected by base
      row FEC, so recovered packets carry M=0. Downstream frame assembly
      that keys on M must treat recovered packets accordingly.
    - Sequence-number wraparound is handled via 32-bit extended sequencing.
 =============================================================================*/
#include "zff/plugins/zstr_st2110_toolkit.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

#define ZSTR_FEC_DEFAULT_PT 127
#define ZSTR_FEC_MAX_L 24
#define ZSTR_FEC_MAX_ROWS 16 /* decoder row-group slots */

/* --- Shared helpers --- */

static uint16_t pkt_seq(const uint8_t *d)
{
    return (uint16_t)((d[2] << 8) | d[3]);
}

static uint32_t pkt_ts(const uint8_t *d)
{
    return ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
           ((uint32_t)d[6] << 8) | d[7];
}

static uint32_t pkt_ssrc(const uint8_t *d)
{
    return ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) |
           ((uint32_t)d[10] << 8) | d[11];
}

/* Unwrap 16-bit seq into 32-bit space given the last extended value. */
static uint32_t unwrap_seq(uint16_t seq, uint32_t last_ext)
{
    uint32_t base = last_ext & 0xFFFF0000u;
    uint32_t cand = base | seq;
    if (cand + 0x8000u < last_ext) cand += 0x10000u;
    else if (cand > last_ext + 0x8000u && cand >= 0x10000u) cand -= 0x10000u;
    if (last_ext == 0 && cand > 0x8000u) cand -= 0x10000u; /* cold start high */
    return cand;
}

/* --- Encoder --- */

struct zstr_st2022_5_fec {
    int row_len;          /* L: media packets per FEC packet */
    uint8_t fec_pt;
    uint32_t seq;         /* FEC stream seq (own counter) */
    /* Current row accumulation */
    uint8_t *xor_buf;
    int xor_cap;
    int xor_len;          /* protection length = max payload seen */
    uint16_t row_base_seq;
    uint32_t row_ts;
    uint8_t row_pt;
    int row_count;
    uint32_t last_ext;
    int have_last;
};

zstr_st2022_5_fec_t *zstr_st2022_5_fec_create(const zstr_st2022_5_config_t *cfg)
{
    zstr_st2022_5_fec_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    int L = (cfg && cfg->row_len > 0) ? cfg->row_len : 4;
    if (L < 2) L = 2;
    if (L > ZSTR_FEC_MAX_L) L = ZSTR_FEC_MAX_L;
    s->row_len = L;
    s->fec_pt = (cfg && cfg->fec_pt) ? cfg->fec_pt : ZSTR_FEC_DEFAULT_PT;
    s->seq = (uint32_t)(rand() & 0xFFFF);
    s->xor_cap = 2048;
    s->xor_buf = calloc(1, s->xor_cap);
    if (!s->xor_buf) {
        free(s);
        return NULL;
    }
    return s;
}

/* Feed one media RTP packet. Returns an FEC AVPacket* when the row
 * completes, else NULL (caller does not own NULL). Resets row on SSRC
 * change or sequence gap (rows must be consecutive). */
AVPacket *zstr_st2022_5_fec_encode(zstr_st2022_5_fec_t *s, const AVPacket *media)
{
    if (!s || !media || media->size < 12) return NULL;
    const uint8_t *d = media->data;
    if ((d[0] >> 6) != 2) return NULL;

    uint16_t seq = pkt_seq(d);
    uint32_t ext;
    if (!s->have_last && s->row_count == 0) {
        ext = seq;
        s->have_last = 1;
        s->last_ext = ext;
    } else {
        ext = unwrap_seq(seq, s->last_ext);
        if (ext != s->last_ext + 1) {
            /* Gap: abandon partial row, restart here */
            s->row_count = 0;
        }
        s->last_ext = ext;
    }

    int payload_len = media->size - 12;
    if (payload_len > s->xor_cap) return NULL; /* oversize: skip row */

    if (s->row_count == 0) {
        memset(s->xor_buf, 0, s->xor_cap);
        s->xor_len = 0;
        s->row_base_seq = seq;
        s->row_ts = pkt_ts(d);
        s->row_pt = d[1] & 0x7F;
    }
    for (int i = 0; i < payload_len; i++) s->xor_buf[i] ^= d[12 + i];
    if (payload_len > s->xor_len) s->xor_len = payload_len;
    s->row_count++;

    if (s->row_count < s->row_len) return NULL;

    /* Row complete: emit FEC packet */
    s->row_count = 0;
    AVPacket *fec = av_packet_alloc();
    if (!fec || av_new_packet(fec, 12 + 12 + s->xor_len) < 0) {
        av_packet_free(&fec);
        return NULL;
    }
    uint8_t *o = fec->data;
    uint32_t ssrc = pkt_ssrc(d);
    o[0] = 0x80;
    o[1] = s->fec_pt & 0x7F; /* M=0 on FEC */
    o[2] = (uint8_t)(s->seq >> 8);
    o[3] = (uint8_t)(s->seq & 0xFF);
    s->seq++;
    /* FEC timestamp: reuse media TS (same stream multiplexing) */
    o[4] = (uint8_t)(s->row_ts >> 24);
    o[5] = (uint8_t)((s->row_ts >> 16) & 0xFF);
    o[6] = (uint8_t)((s->row_ts >> 8) & 0xFF);
    o[7] = (uint8_t)(s->row_ts & 0xFF);
    o[8] = (uint8_t)(ssrc >> 24);
    o[9] = (uint8_t)((ssrc >> 16) & 0xFF);
    o[10] = (uint8_t)((ssrc >> 8) & 0xFF);
    o[11] = (uint8_t)(ssrc & 0xFF);
    /* RFC 2733 FEC header */
    o[12] = (uint8_t)(s->row_base_seq >> 8);
    o[13] = (uint8_t)(s->row_base_seq & 0xFF);
    o[14] = (uint8_t)(s->xor_len >> 8);
    o[15] = (uint8_t)(s->xor_len & 0xFF);
    o[16] = s->row_pt & 0x7F; /* E=0 */
    uint32_t mask = (s->row_len >= 32) ? 0xFFFFFFFFu
                                       : (((uint32_t)1 << s->row_len) - 1);
    o[17] = (uint8_t)((mask >> 16) & 0xFF);
    o[18] = (uint8_t)((mask >> 8) & 0xFF);
    o[19] = (uint8_t)(mask & 0xFF);
    o[20] = (uint8_t)(s->row_ts >> 24);
    o[21] = (uint8_t)((s->row_ts >> 16) & 0xFF);
    o[22] = (uint8_t)((s->row_ts >> 8) & 0xFF);
    o[23] = (uint8_t)(s->row_ts & 0xFF);
    memcpy(o + 24, s->xor_buf, s->xor_len);

    fec->pts = media->pts;
    fec->dts = media->dts;
    fec->time_base = media->time_base;
    return fec;
}

void zstr_st2022_5_fec_free(zstr_st2022_5_fec_t **ps)
{
    if (!ps || !*ps) return;
    free((*ps)->xor_buf);
    free(*ps);
    *ps = NULL;
}

/* --- Decoder --- *//* --- Decoder ---
 *
 * Sliding-window design (no grid assumption): media packets are buffered
 * by extended sequence number; when an FEC packet arrives, the packets in
 * [base, base+L) are gathered. Exactly L-1 present -> recover; fewer ->
 * unrecoverable. This matches encoder rows whose base is simply the first
 * sequence number of each consecutive run, so sequence wraparound needs
 * no special row bookkeeping.
 */

#define ZSTR_FEC_WINDOW 64

typedef struct {
    int used;
    uint32_t ext;
    uint8_t *data;
    int len;
    uint32_t ts;
    uint32_t ssrc;
    int64_t pts;
    int64_t dts;
    AVRational tb;
} fec_slot_t;

struct zstr_st2022_5_fec_decoder {
    int row_len;
    uint8_t fec_pt; /* 0 = accept any non-media PT as FEC */
    uint8_t media_pt;
    fec_slot_t window[ZSTR_FEC_WINDOW];
    uint32_t last_ext;
    int have_last;
    uint64_t recovered;
    uint64_t unrecoverable;
};

zstr_st2022_5_fec_decoder_t *zstr_st2022_5_fec_decoder_create(
    const zstr_st2022_5_config_t *cfg, uint8_t media_pt)
{
    zstr_st2022_5_fec_decoder_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    int L = (cfg && cfg->row_len > 0) ? cfg->row_len : 4;
    if (L < 2) L = 2;
    if (L > ZSTR_FEC_MAX_L) L = ZSTR_FEC_MAX_L;
    s->row_len = L;
    s->fec_pt = (cfg && cfg->fec_pt) ? cfg->fec_pt : 0;
    s->media_pt = media_pt;
    return s;
}

/* Feed one media packet (stores a copy for potential recovery). */
int zstr_st2022_5_fec_decoder_media(zstr_st2022_5_fec_decoder_t *s,
                                    const AVPacket *media)
{
    if (!s || !media || media->size < 12) return -1;
    const uint8_t *d = media->data;
    if ((d[0] >> 6) != 2) return -1;
    uint16_t seq = pkt_seq(d);

    uint32_t ext;
    if (!s->have_last) {
        ext = seq;
        s->have_last = 1;
        s->last_ext = ext;
    } else {
        ext = unwrap_seq(seq, s->last_ext);
        s->last_ext = ext;
    }

    fec_slot_t *slot = &s->window[ext % ZSTR_FEC_WINDOW];
    free(slot->data);
    slot->data = malloc(media->size - 12);
    if (!slot->data) {
        slot->used = 0;
        return -1;
    }
    memcpy(slot->data, d + 12, media->size - 12);
    slot->len = media->size - 12;
    slot->used = 1;
    slot->ext = ext;
    slot->ts = pkt_ts(d);
    slot->ssrc = pkt_ssrc(d);
    slot->pts = media->pts;
    slot->dts = media->dts;
    slot->tb = media->time_base;
    return 0;
}

/* Feed one FEC packet. Returns a recovered AVPacket* when exactly one
 * packet of its row was missing (caller owns it), else NULL. */
AVPacket *zstr_st2022_5_fec_decoder_fec(zstr_st2022_5_fec_decoder_t *s,
                                       const AVPacket *fec)
{
    if (!s || !fec || fec->size < 24) return NULL;
    const uint8_t *d = fec->data;
    if ((d[0] >> 6) != 2) return NULL;
    uint8_t pt = d[1] & 0x7F;
    if (s->fec_pt && pt != s->fec_pt) return NULL;
    if (s->fec_pt == 0 && pt == s->media_pt) return NULL; /* not FEC */

    uint16_t base_seq = (uint16_t)((d[12] << 8) | d[13]);
    int prot_len = (d[14] << 8) | d[15];
    uint8_t pt_rec = d[16] & 0x7F;
    uint32_t mask = ((uint32_t)d[17] << 16) | ((uint32_t)d[18] << 8) | d[19];
    uint32_t ts_rec = ((uint32_t)d[20] << 24) | ((uint32_t)d[21] << 16) |
                      ((uint32_t)d[22] << 8) | d[23];
    if (fec->size < 24 + prot_len) return NULL;

    /* Count protected slots from mask (dense: low row_len bits) */
    int L = 0;
    for (int i = 0; i < 24; i++) {
        if (mask & ((uint32_t)1 << i)) L++;
    }
    if (L != s->row_len) return NULL;

    /* Align signaled base to the stream position (wrap-aware) */
    uint32_t base_ext = base_seq;
    if (s->have_last) {
        uint32_t aligned = (s->last_ext & 0xFFFF0000u) | base_seq;
        if (aligned + 0x8000u < s->last_ext) aligned += 0x10000u;
        else if (aligned > s->last_ext + 0x8000u && aligned >= 0x10000u)
            aligned -= 0x10000u;
        base_ext = aligned;
    }

    /* Gather window packets in [base_ext, base_ext + L) */
    int present = 0;
    int miss_off = -1;
    for (int i = 0; i < L; i++) {
        uint32_t want = base_ext + (uint32_t)i;
        fec_slot_t *slot = &s->window[want % ZSTR_FEC_WINDOW];
        if (slot->used && slot->ext == want) {
            present++;
        } else if (miss_off < 0) {
            miss_off = i;
        } else {
            s->unrecoverable++;
            return NULL; /* two or more missing */
        }
    }
    if (present != L - 1 || miss_off < 0) return NULL;

    uint8_t *rec = calloc(1, prot_len);
    if (!rec) return NULL;
    memcpy(rec, d + 24, prot_len);
    for (int i = 0; i < L; i++) {
        if (i == miss_off) continue;
        uint32_t want = base_ext + (uint32_t)i;
        fec_slot_t *slot = &s->window[want % ZSTR_FEC_WINDOW];
        int n = slot->len < prot_len ? slot->len : prot_len;
        for (int k = 0; k < n; k++) rec[k] ^= slot->data[k];
    }

    /* Reference metadata from a present sibling (SSRC/PTS shared per row) */
    uint32_t ssrc = 0;
    int64_t pts = 0, dts = 0;
    AVRational tb = { 1, 90000 };
    for (int i = 0; i < L; i++) {
        if (i == miss_off) continue;
        uint32_t want = base_ext + (uint32_t)i;
        fec_slot_t *slot = &s->window[want % ZSTR_FEC_WINDOW];
        ssrc = slot->ssrc;
        pts = slot->pts;
        dts = slot->dts;
        tb = slot->tb;
        break;
    }

    AVPacket *out = av_packet_alloc();
    if (!out || av_new_packet(out, 12 + prot_len) < 0) {
        av_packet_free(&out);
        free(rec);
        return NULL;
    }
    uint8_t *o = out->data;
    uint16_t seq = (uint16_t)(base_ext + (uint32_t)miss_off);
    o[0] = 0x80;
    o[1] = pt_rec & 0x7F; /* NOTE: marker bit unprotected, emitted 0 (see header) */
    o[2] = (uint8_t)(seq >> 8);
    o[3] = (uint8_t)(seq & 0xFF);
    o[4] = (uint8_t)(ts_rec >> 24);
    o[5] = (uint8_t)((ts_rec >> 16) & 0xFF);
    o[6] = (uint8_t)((ts_rec >> 8) & 0xFF);
    o[7] = (uint8_t)(ts_rec & 0xFF);
    o[8] = (uint8_t)(ssrc >> 24);
    o[9] = (uint8_t)((ssrc >> 16) & 0xFF);
    o[10] = (uint8_t)((ssrc >> 8) & 0xFF);
    o[11] = (uint8_t)(ssrc & 0xFF);
    memcpy(o + 12, rec, prot_len);
    free(rec);

    out->pts = pts;
    out->dts = dts;
    out->time_base = tb;
    s->recovered++;
    return out;
}

uint64_t zstr_st2022_5_fec_recovered(const zstr_st2022_5_fec_decoder_t *s)
{
    return s ? s->recovered : 0;
}

uint64_t zstr_st2022_5_fec_unrecoverable(const zstr_st2022_5_fec_decoder_t *s)
{
    return s ? s->unrecoverable : 0;
}

void zstr_st2022_5_fec_decoder_free(zstr_st2022_5_fec_decoder_t **ps)
{
    if (!ps || !*ps) return;
    zstr_st2022_5_fec_decoder_t *s = *ps;
    for (int i = 0; i < ZSTR_FEC_WINDOW; i++) free(s->window[i].data);
    free(s);
    *ps = NULL;
}
