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

typedef struct {
    const char *text;          /**< Initial static text string */
    const char *font_path;     /**< Path to TTF/OTF font file (NULL = default system font) */
    int font_size;             /**< Font pixel height (default: 32) */
    int x;                     /**< Left X coordinate in pixels */
    int y;                     /**< Top Y coordinate in pixels */
    uint32_t text_color;       /**< Text color in 0xRRGGBBAA format (default: 0xFFFFFFFF white) */
    bool draw_box;             /**< Draw background bounding box */
    uint32_t box_color;        /**< Box color in 0xRRGGBBAA format (default: 0x00000080) */
    bool show_timecode;        /**< Display dynamic timecode (HH:MM:SS.mmm or HH:MM:SS:FF) */
    AVRational timecode_rate;  /**< Frame rate for timecode frames field (e.g. 25/1, 30/1) */
} zstr_text_overlay_config_t;

/**
 * Allocate and initialize a text overlay instance.
 *
 * @param opt_string Key=value configuration string.
 *                   e.g. "text=Hello:x=50:y=50:font_size=32:timecode=1:box=1"
 *                   Options:
 *                     - text: default text
 *                     - font_path, fontfile: path to TTF/OTF file
 *                     - font_size, size: font height in pixels
 *                     - x, y: coordinate offsets
 *                     - color: hex color (e.g. 0xFFFFFFFF)
 *                     - box: 1 to draw background box, 0 to disable
 *                     - boxcolor: hex background box color
 *                     - timecode: 1 to enable dynamic timecode overlay
 * @return Allocated instance, or NULL on error.
 */
zstr_text_overlay_t* zstr_text_overlay_alloc(const char *opt_string);

/**
 * Allocate and initialize with explicit configuration struct.
 */
zstr_text_overlay_t* zstr_text_overlay_create(const zstr_text_overlay_config_t *cfg);

/**
 * Dynamically update the displayed text (thread-safe).
 */
int zstr_text_overlay_set_text(zstr_text_overlay_t *s, const char *text);

/**
 * Set active subtitle with duration and timestamp (thread-safe).
 * The subtitle will automatically expire when frame PTS > end_pts.
 *
 * @param s            Pointer to text overlay.
 * @param text         Subtitle text string.
 * @param start_pts    Start PTS in time_base units.
 * @param duration_pts Duration in time_base units.
 * @param time_base    Time base for PTS calculation (e.g. {1, 1000} or stream time_base).
 */
int zstr_text_overlay_set_subtitle(zstr_text_overlay_t *s, const char *text,
                                  int64_t start_pts, int64_t duration_pts,
                                  AVRational time_base);

/**
 * Dynamically update position (x, y coordinates).
 */
void zstr_text_overlay_set_position(zstr_text_overlay_t *s, int x, int y);

/**
 * Enable or disable dynamic timecode display.
 */
void zstr_text_overlay_enable_timecode(zstr_text_overlay_t *s, bool enable);

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
