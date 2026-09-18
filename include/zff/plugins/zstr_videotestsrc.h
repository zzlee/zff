/*=============================================================================
    zstr_videotestsrc.h — Video Test Source Device (AVInputFormat)
=============================================================================*/
#pragma once

#include <libavformat/avformat.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard FFmpeg AVInputFormat device for zstr_videotestsrc.
 * Use via standard FFmpeg API:
 *   avformat_open_input(&fmt_ctx, "dummy", zff_find_input_format("zstr_videotestsrc"), &options);
 *   av_read_frame(fmt_ctx, pkt);
 */
extern const AVInputFormat ff_zstr_videotestsrc_demuxer;

#ifdef __cplusplus
}
#endif
