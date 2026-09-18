/*=============================================================================
    zstr_st2022_7.c — SMPTE ST 2022-7 Seamless Hitless Redundancy Demuxer
=============================================================================*/
#include "zff/plugins/zstr_st2110.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <libavutil/error.h>

#define SEQ_WINDOW_SIZE 4096

struct zstr_st2022_7_demux {
    pthread_mutex_t lock;
    bool initialized;
    uint16_t highest_seq;
    uint8_t seen_bitmap[8192]; /* 65536 bits */
    uint64_t total_packets;
    uint64_t duplicate_packets;
};

zstr_st2022_7_demux_t* zstr_st2022_7_demux_create(void)
{
    zstr_st2022_7_demux_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    pthread_mutex_init(&s->lock, NULL);
    return s;
}

static inline bool test_and_set_bit(uint8_t *bitmap, uint16_t seq)
{
    int byte_idx = seq >> 3;
    int bit_idx = seq & 7;
    uint8_t mask = (uint8_t)(1 << bit_idx);

    if (bitmap[byte_idx] & mask) {
        return true; /* Already seen */
    }
    bitmap[byte_idx] |= mask;
    return false;
}

static inline void clear_seq_range(uint8_t *bitmap, uint16_t start, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        uint16_t seq = (uint16_t)(start + i);
        bitmap[seq >> 3] &= ~(1 << (seq & 7));
    }
}

int zstr_st2022_7_demux_process(zstr_st2022_7_demux_t *s,
                                int path_idx,
                                const AVPacket *in_pkt,
                                bool *is_duplicate)
{
    (void)path_idx;
    if (!s || !in_pkt || !in_pkt->data || in_pkt->size < 12 || !is_duplicate)
        return AVERROR(EINVAL);

    uint16_t seq = (uint16_t)((in_pkt->data[2] << 8) | in_pkt->data[3]);

    pthread_mutex_lock(&s->lock);
    s->total_packets++;

    if (!s->initialized) {
        s->initialized = true;
        s->highest_seq = seq;
        memset(s->seen_bitmap, 0, sizeof(s->seen_bitmap));
        test_and_set_bit(s->seen_bitmap, seq);
        *is_duplicate = false;
        pthread_mutex_unlock(&s->lock);
        return 0;
    }

    /* Clear ahead if sequence advances significantly to prevent false duplicates on wrap-around */
    int16_t diff = (int16_t)(seq - s->highest_seq);
    if (diff > 0) {
        if (diff > SEQ_WINDOW_SIZE) diff = SEQ_WINDOW_SIZE;
        clear_seq_range(s->seen_bitmap, (uint16_t)(s->highest_seq + 1), (uint16_t)diff);
        s->highest_seq = seq;
    }

    /* Check if duplicate */
    if (test_and_set_bit(s->seen_bitmap, seq)) {
        s->duplicate_packets++;
        *is_duplicate = true;
        pthread_mutex_unlock(&s->lock);
        return 0;
    }

    *is_duplicate = false;
    pthread_mutex_unlock(&s->lock);
    return 0;
}

void zstr_st2022_7_demux_free(zstr_st2022_7_demux_t **ps)
{
    if (!ps || !*ps) return;
    zstr_st2022_7_demux_t *s = *ps;
    pthread_mutex_destroy(&s->lock);
    free(s);
    *ps = NULL;
}
