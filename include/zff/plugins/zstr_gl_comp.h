/*=============================================================================
    zstr_gl_comp.h — OpenGL Video Compositor & Multi-Stream Sink Device
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>
#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZSTR_GL_COMP_MAX_LAYERS 16

typedef struct zstr_gl_comp zstr_gl_comp_t;

typedef struct {
    int x;                 /**< Destination top-left X coordinate on canvas */
    int y;                 /**< Destination top-left Y coordinate on canvas */
    int width;             /**< Destination width (0 = native frame width) */
    int height;            /**< Destination height (0 = native frame height) */
    int z_order;           /**< Stacking order (lower drawn first, higher on top) */
    float alpha;           /**< Opacity: 0.0 (transparent) to 1.0 (fully opaque) */
    bool visible;          /**< Visibility toggle */
    int border_width;      /**< Border thickness in pixels */
    uint32_t border_color; /**< Border RGBA (0xRRGGBBAA) */
} zstr_gl_comp_layer_cfg_t;

typedef struct {
    int canvas_width;          /**< Compositor canvas width in pixels (default: 1920) */
    int canvas_height;         /**< Compositor canvas height in pixels (default: 1080) */
    const char *window_title;  /**< X11 Window title */
    const char *display;       /**< X11 display name (e.g. ":0", ":99", or NULL for default) */
    uint32_t bg_color;         /**< Background canvas color in 0xRRGGBBAA format (default: 0x000000FF) */
    bool fullscreen;           /**< Fullscreen display */
    bool vsync;                /**< VSync swap interval */
    bool is_mock;              /**< Force headless mock / software mode */
} zstr_gl_comp_config_t;

/**
 * Allocate and initialize a video compositor instance.
 */
zstr_gl_comp_t* zstr_gl_comp_create(const zstr_gl_comp_config_t *cfg);

/**
 * Allocate from a key=value option string (e.g. "w=1280:h=720:bg=0x101010FF:mock=0").
 */
zstr_gl_comp_t* zstr_gl_comp_alloc(const char *opt_string);

/**
 * Configure layout and appearance for a specific layer.
 */
int zstr_gl_comp_configure_layer(zstr_gl_comp_t *c, int layer_idx, const zstr_gl_comp_layer_cfg_t *cfg);

/**
 * Push an updated video frame into a specific layer.
 * A reference to the frame or a copy will be retained for composition.
 */
int zstr_gl_comp_set_layer_frame(zstr_gl_comp_t *c, int layer_idx, const AVFrame *frame);

/**
 * Render all visible layers onto the canvas and present to screen (via GLX / X11).
 * In mock/null mode, this performs software composition.
 */
int zstr_gl_comp_render(zstr_gl_comp_t *c);

/**
 * Capture the composited canvas into an output AVFrame (RGB24, RGBA, or YUV420P).
 *
 * @param c   Compositor instance.
 * @param out Output AVFrame (will be allocated with canvas_width/canvas_height if unallocated).
 * @return 0 on success, negative AVERROR on failure.
 */
int zstr_gl_comp_capture(zstr_gl_comp_t *c, AVFrame *out);

/**
 * Free compositor instance and all GL/X11 resources.
 */
void zstr_gl_comp_free(zstr_gl_comp_t **c);

/**
 * FFmpeg AVOutputFormat device definition for "zstr_gl_comp".
 * Allows writing multiple video streams (stream 0, 1, ...) via av_write_frame().
 */
extern const FFOutputFormat ff_zstr_gl_comp_muxer;

#ifdef __cplusplus
}
#endif
