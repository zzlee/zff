/*=============================================================================
    zstr_st2022_5.c — SMPTE ST 2022-5 Row + Column FEC (XOR protection)

    NOTE: zstreamer's st2110_fec.c is an unimplemented passthrough stub
    (TODO comments, no math). This file implements real 1-D row and 2-D
    row+column FEC per RFC 2733 as referenced by ST 2022-5:

    - A row of L consecutive media packets (L <= 24, dense mask) produces
      one FEC packet: SN-base(2) | length-recovery(2) | E+PT-recovery(1) |
      mask(3) | TS-recovery(4) | XOR of (padded) payloads.
    - With col_len D > 1, every D consecutive rows form a matrix; each of
      the L columns additionally produces one FEC packet protecting the
      strided set {base+j, base+j+L, ..., base+j+(D-1)*L} (mask bits set
      accordingly). Constraint: (D-1)*L <= 23 (24-bit RFC 2733 mask).
    - FEC packets ride the SAME SSRC as media (RFC 5109 ULPFEC convention),
      distinguished by payload type (default 127, configurable).
    - Recovery: exactly one missing packet per protected set is rebuilt
      (payload via XOR, PT/TS from recovery fields, length trimmed via
      length-recovery). Row and column recovery are independent passes, so
      a 2-loss-same-row pattern with losses in distinct columns is fully
      recovered via the columns. Denser clusters (needing iteration) are
      beyond v1 scope.
    - Row vs column FEC packets are distinguished by mask shape alone
      (dense low bits vs strided), so no out-of-band signaling is needed.
    - LIMITATION (documented): the RTP marker bit is NOT protected by base
      FEC, so recovered packets carry M=0. Downstream frame assembly
      that keys on M must treat recovered packets accordingly. In-order
      transport is assumed (no reorder retry; same as v1 row code).
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
    int col_len;          /* D: rows per matrix (0/1 = row-only) */
    uint8_t fec_pt;
    uint32_t seq;         /* FEC stream seq (own counter) */
    /* Current row accumulation */
    uint8_t *xor_buf;
    int xor_cap;
    int xor_len;          /* protection length = max payload seen */
    int len_xor;          /* XOR of payload lengths (recovers exact size) */
    uint16_t row_base_seq;
    uint32_t row_ts;
    uint8_t row_pt;
    int row_count;
    uint32_t last_ext;
    int have_last;
    /* Current matrix column accumulation (2-D only) */
    uint8_t *col_xor;     /* L x col_cap bytes */
    int col_cap;
    int *col_max;         /* protection length per column */
    int *col_lenx;        /* length XOR per column */
    uint32_t *col_ts;     /* first-packet TS per column */
    uint8_t *col_pt;      /* PT per column */
    uint16_t mat_base_seq;/* first seq of current matrix */
    int mat_row;          /* completed rows in current matrix */
};

zstr_st2022_5_fec_t *zstr_st2022_5_fec_create(const zstr_st2022_5_config_t *cfg)
{
    zstr_st2022_5_fec_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    int L = (cfg && cfg->row_len > 0) ? cfg->row_len : 4;
    if (L < 2) L = 2;
    if (L > ZSTR_FEC_MAX_L) L = ZSTR_FEC_MAX_L;
    s->row_len = L;
    int D = cfg ? cfg->col_len : 0;
    if (D < 0) D = 0;
    if (D == 1) D = 0; /* degenerate: single-packet "columns" protect nothing */
    if (D > 0 && (D - 1) * L > 23) D = 0; /* 24-bit mask cannot span it */
    s->col_len = D;
    s->fec_pt = (cfg && cfg->fec_pt) ? cfg->fec_pt : ZSTR_FEC_DEFAULT_PT;
    s->seq = (uint32_t)(rand() & 0xFFFF);
    s->xor_cap = 2048;
    s->xor_buf = calloc(1, s->xor_cap);
    if (!s->xor_buf) {
        free(s);
        return NULL;
    }
    if (D > 0) {
        s->col_cap = 2048;
        s->col_xor = calloc((size_t)L, s->col_cap);
        s->col_max = calloc((size_t)L, sizeof(int));
        s->col_lenx = calloc((size_t)L, sizeof(int));
        s->col_ts = calloc((size_t)L, sizeof(uint32_t));
        s->col_pt = calloc((size_t)L, sizeof(uint8_t));
        if (!s->col_xor || !s->col_max || !s->col_lenx ||
            !s->col_ts || !s->col_pt) {
            zstr_st2022_5_fec_free(&s);
            return NULL;
        }
    }
    return s;
}

/* Reset matrix column accumulation (sequence gap abandons the matrix). */
static void fec_matrix_reset(zstr_st2022_5_fec_t *s)
{
    if (s->col_len <= 0) return;
    memset(s->col_xor, 0, (size_t)s->row_len * s->col_cap);
    memset(s->col_max, 0, (size_t)s->row_len * sizeof(int));
    memset(s->col_lenx, 0, (size_t)s->row_len * sizeof(int));
    s->mat_row = 0;
}

/* Build one RFC 2733 FEC packet from explicit protection parameters. */
static AVPacket *emit_fec_packet(zstr_st2022_5_fec_t *s, uint32_t ssrc,
                                 uint16_t base_seq, int len_xor, uint8_t pt,
                                 uint32_t mask, uint32_t ts,
                                 const uint8_t *prot, int prot_len,
                                 const AVPacket *tmpl)
{
    AVPacket *fec = av_packet_alloc();
    if (!fec || av_new_packet(fec, 12 + 12 + prot_len) < 0) {
        av_packet_free(&fec);
        return NULL;
    }
    uint8_t *o = fec->data;
    o[0] = 0x80;
    o[1] = s->fec_pt & 0x7F; /* M=0 on FEC */
    o[2] = (uint8_t)(s->seq >> 8);
    o[3] = (uint8_t)(s->seq & 0xFF);
    s->seq++;
    /* FEC timestamp: reuse media TS (same stream multiplexing) */
    o[4] = (uint8_t)(ts >> 24);
    o[5] = (uint8_t)((ts >> 16) & 0xFF);
    o[6] = (uint8_t)((ts >> 8) & 0xFF);
    o[7] = (uint8_t)(ts & 0xFF);
    o[8] = (uint8_t)(ssrc >> 24);
    o[9] = (uint8_t)((ssrc >> 16) & 0xFF);
    o[10] = (uint8_t)((ssrc >> 8) & 0xFF);
    o[11] = (uint8_t)(ssrc & 0xFF);
    /* RFC 2733 FEC header */
    o[12] = (uint8_t)(base_seq >> 8);
    o[13] = (uint8_t)(base_seq & 0xFF);
    o[14] = (uint8_t)(len_xor >> 8);
    o[15] = (uint8_t)(len_xor & 0xFF);
    o[16] = pt & 0x7F; /* E=0 */
    o[17] = (uint8_t)((mask >> 16) & 0xFF);
    o[18] = (uint8_t)((mask >> 8) & 0xFF);
    o[19] = (uint8_t)(mask & 0xFF);
    o[20] = (uint8_t)(ts >> 24);
    o[21] = (uint8_t)((ts >> 16) & 0xFF);
    o[22] = (uint8_t)((ts >> 8) & 0xFF);
    o[23] = (uint8_t)(ts & 0xFF);
    memcpy(o + 24, prot, prot_len);

    fec->pts = tmpl->pts;
    fec->dts = tmpl->dts;
    fec->time_base = tmpl->time_base;
    return fec;
}

/* Feed one media RTP packet; appends 0+ emitted FEC packets to out[]
 * (capacity 1+L). Returns emitted count, or -1 on invalid input.
 * Row FEC emits at each row completion; with col_len D > 0 each matrix
 * of D rows additionally emits L column FECs at matrix completion. */
static int fec_feed(zstr_st2022_5_fec_t *s, const AVPacket *media,
                    AVPacket **out, int out_cap)
{
    if (!s || !media || media->size < 12) return -1;
    const uint8_t *d = media->data;
    if ((d[0] >> 6) != 2) return -1;
    int nemitted = 0;

    uint16_t seq = pkt_seq(d);
    uint32_t ext;
    if (!s->have_last && s->row_count == 0) {
        ext = seq;
        s->have_last = 1;
        s->last_ext = ext;
    } else {
        ext = unwrap_seq(seq, s->last_ext);
        if (ext != s->last_ext + 1) {
            /* Gap: abandon partial row and matrix, restart here */
            s->row_count = 0;
            fec_matrix_reset(s);
        }
        s->last_ext = ext;
    }

    int payload_len = media->size - 12;
    if (payload_len > s->xor_cap) return -1; /* oversize: skip row */
    if (s->col_len > 0 && payload_len > s->col_cap) {
        fec_matrix_reset(s);
        s->row_count = 0;
        return -1;
    }

    /* Matrix start: first packet of a row that opens a matrix */
    if (s->col_len > 0 && s->row_count == 0 && s->mat_row == 0)
        s->mat_base_seq = seq;

    int col = s->row_count; /* 0-based position of this packet in its row */
    if (s->row_count == 0) {
        memset(s->xor_buf, 0, s->xor_cap);
        s->xor_len = 0;
        s->len_xor = 0;
        s->row_base_seq = seq;
        s->row_ts = pkt_ts(d);
        s->row_pt = d[1] & 0x7F;
    }
    for (int i = 0; i < payload_len; i++) s->xor_buf[i] ^= d[12 + i];
    if (payload_len > s->xor_len) s->xor_len = payload_len;
    s->len_xor ^= payload_len;

    if (s->col_len > 0) {
        uint8_t *cbuf = s->col_xor + (size_t)col * s->col_cap;
        if (s->mat_row == 0) {
            /* First matrix row: latch per-column TS/PT (each column
             * appears exactly once here) */
            s->col_ts[col] = pkt_ts(d);
            s->col_pt[col] = d[1] & 0x7F;
        }
        for (int i = 0; i < payload_len; i++) cbuf[i] ^= d[12 + i];
        if (payload_len > s->col_max[col]) s->col_max[col] = payload_len;
        s->col_lenx[col] ^= payload_len;
    }
    s->row_count++;

    if (s->row_count < s->row_len) return 0;

    /* Row complete */
    s->row_count = 0;
    uint32_t ssrc = pkt_ssrc(d);
    uint32_t row_mask = (s->row_len >= 32) ? 0xFFFFFFFFu
                                           : (((uint32_t)1 << s->row_len) - 1);
    AVPacket *row_fec = emit_fec_packet(s, ssrc, s->row_base_seq, s->len_xor,
                                        s->row_pt, row_mask, s->row_ts,
                                        s->xor_buf, s->xor_len, media);
    if (row_fec && nemitted < out_cap) out[nemitted++] = row_fec;
    else av_packet_free(&row_fec);

    if (s->col_len > 0) {
        s->mat_row++;
        if (s->mat_row >= s->col_len) {
            /* Matrix complete: emit one column FEC per column */
            for (int j = 0; j < s->row_len; j++) {
                uint32_t mask = 0;
                for (int i = 0; i < s->col_len; i++)
                    mask |= (uint32_t)1 << (j + i * s->row_len);
                uint16_t base = (uint16_t)(s->mat_base_seq + j);
                AVPacket *cf = emit_fec_packet(s, ssrc, base, s->col_lenx[j],
                                              s->col_pt[j], mask, s->col_ts[j],
                                              s->col_xor + (size_t)j * s->col_cap,
                                              s->col_max[j], media);
                if (cf && nemitted < out_cap) out[nemitted++] = cf;
                else av_packet_free(&cf);
            }
            fec_matrix_reset(s);
        }
    }
    return nemitted;
}

/* Feed one media RTP packet. Returns an FEC AVPacket* when the row
 * completes, else NULL (caller does not own NULL). Resets row on SSRC
 * change or sequence gap (rows must be consecutive).
 *
 * Row-only API: with a matrix config (col_len > 0) row FECs are still
 * returned here, but column FECs are NOT (use fec_encode_matrix);
 * configuring D > 0 while using this call yields row protection only. */
AVPacket *zstr_st2022_5_fec_encode(zstr_st2022_5_fec_t *s, const AVPacket *media)
{
    AVPacket *out[1 + ZSTR_FEC_MAX_L];
    int n = fec_feed(s, media, out, 1);
    if (n < 0) return NULL;
    /* n >= 1 here means a row completed; out[0] is the row FEC.
     * (With col_len > 0 a matrix may also have completed; the extra
     * column packets cannot be expressed by this signature and are
     * dropped — use fec_encode_matrix for 2-D protection.) */
    for (int i = 1; i < n; i++) av_packet_free(&out[i]);
    return n > 0 ? out[0] : NULL;
}

/* Matrix (2-D) encode: feed one media packet, collect every FEC packet
 * emitted by this call (row completion and/or matrix completion) into
 * *out_pkts (malloc'd array, caller frees each packet and the array;
 * NULL with *nb_out_pkts == 0 when nothing emitted). */
int zstr_st2022_5_fec_encode_matrix(zstr_st2022_5_fec_t *s, const AVPacket *media,
                                    AVPacket ***out_pkts, int *nb_out_pkts)
{
    if (!s || !media || !out_pkts || !nb_out_pkts) return AVERROR(EINVAL);
    *out_pkts = NULL;
    *nb_out_pkts = 0;
    AVPacket *tmp[1 + ZSTR_FEC_MAX_L];
    int n = fec_feed(s, media, tmp, 1 + ZSTR_FEC_MAX_L);
    if (n < 0) return AVERROR(EINVAL);
    if (n == 0) return 0;
    AVPacket **arr = calloc((size_t)n, sizeof(AVPacket *));
    if (!arr) {
        for (int i = 0; i < n; i++) av_packet_free(&tmp[i]);
        return AVERROR(ENOMEM);
    }
    for (int i = 0; i < n; i++) arr[i] = tmp[i];
    *out_pkts = arr;
    *nb_out_pkts = n;
    return 0;
}

void zstr_st2022_5_fec_free(zstr_st2022_5_fec_t **ps)
{
    if (!ps || !*ps) return;
    free((*ps)->xor_buf);
    free((*ps)->col_xor);
    free((*ps)->col_max);
    free((*ps)->col_lenx);
    free((*ps)->col_ts);
    free((*ps)->col_pt);
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
    int col_len;      /* D: rows per matrix (0 = row-only) */
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
    int D = cfg ? cfg->col_len : 0;
    if (D < 0) D = 0;
    if (D == 1) D = 0;
    if (D > 0 && (D - 1) * L > 23) D = 0;
    s->col_len = D;
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
/* Recover exactly-one-missing from an explicit member list.
 * members[] holds extended seqs (count n); returns recovered packet or
 * NULL (unrecoverable / invalid). Shared by row (dense) and column
 * (strided) paths. */
static AVPacket *recover_one_missing(zstr_st2022_5_fec_decoder_t *s,
                                     const uint8_t *fec_data, int fec_size,
                                     const uint32_t *members, int n)
{
    const uint8_t *d = fec_data;
    int len_rec = (d[14] << 8) | d[15];
    int prot_len = fec_size - 24;
    uint8_t pt_rec = d[16] & 0x7F;
    uint32_t ts_rec = ((uint32_t)d[20] << 24) | ((uint32_t)d[21] << 16) |
                      ((uint32_t)d[22] << 8) | d[23];

    int present = 0;
    int miss_idx = -1;
    for (int i = 0; i < n; i++) {
        fec_slot_t *slot = &s->window[members[i] % ZSTR_FEC_WINDOW];
        if (slot->used && slot->ext == members[i]) {
            present++;
        } else if (miss_idx < 0) {
            miss_idx = i;
        } else {
            s->unrecoverable++;
            return NULL; /* two or more missing */
        }
    }
    if (present != n - 1 || miss_idx < 0) return NULL;

    /* Exact missing length = recovery XOR present lengths */
    int exact_len = len_rec;
    for (int i = 0; i < n; i++) {
        if (i == miss_idx) continue;
        fec_slot_t *slot = &s->window[members[i] % ZSTR_FEC_WINDOW];
        exact_len ^= slot->len;
    }
    if (exact_len < 0 || exact_len > prot_len) return NULL;

    uint8_t *rec = calloc(1, prot_len);
    if (!rec) return NULL;
    memcpy(rec, d + 24, prot_len);
    for (int i = 0; i < n; i++) {
        if (i == miss_idx) continue;
        fec_slot_t *slot = &s->window[members[i] % ZSTR_FEC_WINDOW];
        int k = slot->len < prot_len ? slot->len : prot_len;
        for (int b = 0; b < k; b++) rec[b] ^= slot->data[b];
    }

    /* Reference metadata from a present sibling */
    uint32_t ssrc = 0;
    int64_t pts = 0, dts = 0;
    AVRational tb = { 1, 90000 };
    for (int i = 0; i < n; i++) {
        if (i == miss_idx) continue;
        fec_slot_t *slot = &s->window[members[i] % ZSTR_FEC_WINDOW];
        ssrc = slot->ssrc;
        pts = slot->pts;
        dts = slot->dts;
        tb = slot->tb;
        break;
    }

    AVPacket *out = av_packet_alloc();
    if (!out || av_new_packet(out, 12 + exact_len) < 0) {
        av_packet_free(&out);
        free(rec);
        return NULL;
    }
    uint8_t *o = out->data;
    uint16_t seq = (uint16_t)members[miss_idx];
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
    memcpy(o + 12, rec, exact_len);
    free(rec);

    out->pts = pts;
    out->dts = dts;
    out->time_base = tb;
    s->recovered++;
    return out;
}

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
    uint32_t mask = ((uint32_t)d[17] << 16) | ((uint32_t)d[18] << 8) | d[19];

    /* Align signaled base to the stream position (wrap-aware) */
    uint32_t base_ext = base_seq;
    if (s->have_last) {
        uint32_t aligned = (s->last_ext & 0xFFFF0000u) | base_seq;
        if (aligned + 0x8000u < s->last_ext) aligned += 0x10000u;
        else if (aligned > s->last_ext + 0x8000u && aligned >= 0x10000u)
            aligned -= 0x10000u;
        base_ext = aligned;
    }

    /* Classify by mask shape: dense low L bits = row; strided = column */
    uint32_t row_mask = (s->row_len >= 32) ? 0xFFFFFFFFu
                                           : (((uint32_t)1 << s->row_len) - 1);
    if (mask == row_mask) {
        uint32_t members[ZSTR_FEC_MAX_L];
        for (int i = 0; i < s->row_len; i++)
            members[i] = base_ext + (uint32_t)i;
        return recover_one_missing(s, d, fec->size, members, s->row_len);
    }

    if (s->col_len > 1) {
        /* Column j of D rows: bits {j, j+L, ..., j+(D-1)*L} */
        for (int j = 0; j < s->row_len; j++) {
            uint32_t cmask = 0;
            for (int i = 0; i < s->col_len; i++)
                cmask |= (uint32_t)1 << (j + i * s->row_len);
            if (mask != cmask) continue;
            /* Column base already includes +j (mat_base + j); stride by L */
            uint32_t members[ZSTR_FEC_MAX_L];
            for (int i = 0; i < s->col_len; i++)
                members[i] = base_ext + (uint32_t)i * (uint32_t)s->row_len;
            return recover_one_missing(s, d, fec->size, members, s->col_len);
        }
    }
    return NULL; /* unknown mask shape */
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
