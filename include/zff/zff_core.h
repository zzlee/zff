/*=============================================================================
    zff_core.h — zff Core Bridge SDK Public Definitions
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Custom FourCC SideData Identifiers ──────────────────────────────────── */
#define ZSTR_TAG_NVBUF ((enum AVFrameSideDataType)MKTAG('N', 'V', 'B', 'F'))
#define ZSTR_TAG_PTP   ((enum AVFrameSideDataType)MKTAG('P', 'T', 'P', 'T'))
#define ZSTR_TAG_BBOX  ((enum AVFrameSideDataType)MKTAG('B', 'B', 'O', 'X'))
#define ZSTR_TAG_USER  ((enum AVFrameSideDataType)MKTAG('Z', 'U', 'S', 'R'))
#define ZSTR_TAG_DMAB  ((enum AVFrameSideDataType)MKTAG('D', 'M', 'A', 'B'))

/* ── Global Discovery & Registration ─────────────────────────────────────── */

/**
 * Register all zff native FFmpeg plugins.
 * Automatically called when libzff-plugins is loaded, or can be called explicitly.
 */
int zff_plugins_register_all(void);

/**
 * Register a custom AVInputFormat into the zff registry.
 */
int zff_register_input_format(const AVInputFormat *fmt);

/**
 * Find an AVInputFormat by name.
 * Searches zff registered input formats first (e.g. "zstr_videotestsrc", "zstr_audiotestsrc"),
 * and falls back to standard FFmpeg av_find_input_format().
 */
const AVInputFormat* zff_find_input_format(const char *name);

/**
 * Register a custom AVOutputFormat into the zff registry.
 */
int zff_register_output_format(const AVOutputFormat *fmt);

/**
 * Find an AVOutputFormat by name.
 * Searches zff registered output formats first (e.g. "zstr_v4l2_sink", "zstr_alsa_sink"),
 * and falls back to standard FFmpeg av_guess_format().
 */
const AVOutputFormat* zff_find_output_format(const char *name);

#ifdef __cplusplus
}
#endif
