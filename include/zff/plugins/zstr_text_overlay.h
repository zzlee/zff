/*=============================================================================
    zstr_text_overlay.h — High-Performance Text, Subtitle & Timecode Overlay
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zstr_text_overlay zstr_text_overlay_t;

/**
 * Allocate and initialize a text overlay instance.
 *
 * @param opt_string Key=value configuration string.
 *                   e.g. "text=Hello:x=50:y=50:font_size=32:timecode=1:box=1:color=0xFFFFFFFF"
 *                   Options:
 *                     - text: default text
 *                     - font_path, fontfile: path to TTF/OTF file
 *                     - font_size, size: font height in pixels (default 32)
 *                     - x, y: coordinate offsets
 *                     - color: hex color in 0xRRGGBBAA format
 *                     - box: 1 to draw background box, 0 to disable
 *                     - boxcolor: hex background box color
 *                     - timecode: 1 to enable dynamic timecode overlay
 * @return Allocated instance, or NULL on error.
 */
zstr_text_overlay_t* zstr_text_overlay_alloc(const char *opt_string);

/**
 * Dynamically configure runtime parameters via key=value string (thread-safe).
 * Supported parameters:
 *   - "text=<string>"
 *   - "x=<int>", "y=<int>"
 *   - "timecode=<0|1>"
 *   - "color=<0xRRGGBBAA>"
 *   - "font_size=<int>"
 *
 * Multiple options can be separated by ':' or ','.
 */
int zstr_text_overlay_set_param(zstr_text_overlay_t *s, const char *param_str);

/**
 * Set active subtitle with duration and timestamp (thread-safe).
 * The subtitle will automatically expire when frame PTS > end_pts.
 *
 * @param s            Pointer to text overlay.
 * @param text         Subtitle text string (or NULL to clear).
 * @param start_pts    Start PTS in time_base units.
 * @param duration_pts Duration in time_base units.
 * @param time_base    Time base for PTS calculation (e.g. {1, 1000} or stream time_base).
 */
int zstr_text_overlay_set_subtitle(zstr_text_overlay_t *s, const char *text,
                                  int64_t start_pts, int64_t duration_pts,
                                  AVRational time_base);

/**
 * Process and render text overlay onto an AVFrame.
 * Supports in-place rendering if in == out, or creates a newly rendered frame.
 * Supported pixel formats:
 *   - AV_PIX_FMT_YUV420P
 *   - AV_PIX_FMT_NV12
 *   - AV_PIX_FMT_RGB24
 *   - AV_PIX_FMT_RGBA
 *
 * Propagates PTS, durations, time_base, and side_data (including PTP tags).
 *
 * @param s   Pointer to text overlay.
 * @param in  Input video AVFrame.
 * @param out Output video AVFrame (can be same as in for in-place rendering).
 * @return 0 on success, negative AVERROR on failure.
 */
int zstr_text_overlay_process(zstr_text_overlay_t *s, const AVFrame *in, AVFrame *out);

/**
 * Free text overlay instance and FreeType resources.
 */
void zstr_text_overlay_free(zstr_text_overlay_t **s);

#ifdef __cplusplus
}
#endif
