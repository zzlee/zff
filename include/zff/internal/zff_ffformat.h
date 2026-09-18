/*=============================================================================
    zff_ffformat.h — Internal FFmpeg Format Adapter for Outdevs/Muxers
=============================================================================*/
#pragma once

#include <libavformat/avformat.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AVDeviceInfoList;

typedef struct FFOutputFormat {
    AVOutputFormat p;
    int priv_data_size;
    int flags_internal;
    int (*write_header)(AVFormatContext *);
    int (*write_packet)(AVFormatContext *, AVPacket *pkt);
    int (*write_trailer)(AVFormatContext *);
    int (*interleave_packet)(AVFormatContext *s, AVPacket *pkt, int flush, int has_packet);
    int (*query_codec)(enum AVCodecID id, int std_compliance);
    void (*get_output_timestamp)(AVFormatContext *s, int stream, int64_t *dts, int64_t *wall);
    int (*control_message)(AVFormatContext *s, int type, void *data, size_t data_size);
    int (*write_uncoded_frame)(AVFormatContext *, int stream_index, struct AVFrame **frame, unsigned flags);
    int (*get_device_list)(AVFormatContext *s, struct AVDeviceInfoList *device_list);
    int (*init)(AVFormatContext *);
    void (*deinit)(AVFormatContext *);
    int (*check_bitstream)(AVFormatContext *s, AVStream *st, const AVPacket *pkt);
} FFOutputFormat;

#ifdef __cplusplus
}
#endif
