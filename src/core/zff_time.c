/*=============================================================================
    zff_time.c — PTP Timestamps & AVRational Timebase Conversion
=============================================================================*/
#include "zff/zff_time.h"
#include <string.h>
#include <libavutil/mathematics.h>

int zff_frame_set_ptp(AVFrame *frame, const zff_ptp_time_t *ptp) {
    if (!frame || !ptp) return -1;
    AVFrameSideData *sd = av_frame_new_side_data(frame, ZSTR_TAG_PTP, sizeof(zff_ptp_time_t));
    if (!sd) return -1;
    memcpy(sd->data, ptp, sizeof(zff_ptp_time_t));
    return 0;
}

int zff_frame_get_ptp(const AVFrame *frame, zff_ptp_time_t *out_ptp) {
    if (!frame || !out_ptp) return -1;
    AVFrameSideData *sd = av_frame_get_side_data(frame, ZSTR_TAG_PTP);
    if (!sd || sd->size < sizeof(zff_ptp_time_t)) return -1;
    memcpy(out_ptp, sd->data, sizeof(zff_ptp_time_t));
    return 0;
}

int zff_packet_set_ptp(AVPacket *pkt, const zff_ptp_time_t *ptp) {
    if (!pkt || !ptp) return -1;
    uint8_t *data = av_packet_new_side_data(pkt, (enum AVPacketSideDataType)ZSTR_TAG_PTP, sizeof(zff_ptp_time_t));
    if (!data) return -1;
    memcpy(data, ptp, sizeof(zff_ptp_time_t));
    return 0;
}

int zff_packet_get_ptp(const AVPacket *pkt, zff_ptp_time_t *out_ptp) {
    if (!pkt || !out_ptp) return -1;
    size_t size = 0;
    uint8_t *data = av_packet_get_side_data(pkt, (enum AVPacketSideDataType)ZSTR_TAG_PTP, &size);
    if (!data || size < sizeof(zff_ptp_time_t)) return -1;
    memcpy(out_ptp, data, sizeof(zff_ptp_time_t));
    return 0;
}

int64_t zff_ptp_to_pts(uint64_t tai_nanos, AVRational time_base) {
    if (time_base.num == 0 || time_base.den == 0) return 0;
    /* Convert nanoseconds (1/1,000,000,000 sec) to target AVRational time_base */
    AVRational nanos_base = { 1, 1000000000 };
    return av_rescale_q((int64_t)tai_nanos, nanos_base, time_base);
}
