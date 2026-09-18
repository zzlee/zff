/*=============================================================================
    zff_time.h — IEEE 1588 PTP & High-Precision Timestamp Helpers
=============================================================================*/
#pragma once

#include "zff_core.h"
#include <libavutil/rational.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t tai_nanoseconds;
    uint32_t domain;
} zff_ptp_time_t;

/**
 * Attach PTP metadata to an AVFrame using FourCC side data (ZSTR_TAG_PTP).
 */
int zff_frame_set_ptp(AVFrame *frame, const zff_ptp_time_t *ptp);

/**
 * Retrieve PTP metadata from an AVFrame.
 */
int zff_frame_get_ptp(const AVFrame *frame, zff_ptp_time_t *out_ptp);

/**
 * Attach PTP metadata to an AVPacket using FourCC side data.
 */
int zff_packet_set_ptp(AVPacket *pkt, const zff_ptp_time_t *ptp);

/**
 * Retrieve PTP metadata from an AVPacket.
 */
int zff_packet_get_ptp(const AVPacket *pkt, zff_ptp_time_t *out_ptp);

/**
 * Convert PTP nanoseconds to FFmpeg PTS using an AVRational time_base.
 */
int64_t zff_ptp_to_pts(uint64_t tai_nanos, AVRational time_base);

#ifdef __cplusplus
}
#endif
