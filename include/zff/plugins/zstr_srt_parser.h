/*=============================================================================
    zstr_srt_parser.h — SubRip (.srt) Subtitle File/Memory Parser
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <libavutil/rational.h>
#include "zff/plugins/zstr_text_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zstr_srt_parser zstr_srt_parser_t;

typedef struct {
    int index;
    int64_t start_ms;
    int64_t duration_ms;
    char *text;
} zstr_srt_subtitle_entry_t;

/**
 * Parse an SRT subtitle file from disk.
 */
zstr_srt_parser_t* zstr_srt_parser_create_from_file(const char *path);

/**
 * Parse an SRT subtitle formatted string from memory.
 */
zstr_srt_parser_t* zstr_srt_parser_create_from_memory(const char *data, size_t len);

/**
 * Get total number of subtitle entries parsed.
 */
int zstr_srt_parser_get_count(const zstr_srt_parser_t *p);

/**
 * Get subtitle entry by index (0-based).
 */
int zstr_srt_parser_get_entry(const zstr_srt_parser_t *p, int index,
                              int64_t *start_ms, int64_t *duration_ms,
                              const char **text);

/**
 * Find active subtitle text at given timestamp (in milliseconds).
 * Returns NULL if no subtitle is active.
 */
const char* zstr_srt_parser_find_at_time(const zstr_srt_parser_t *p, int64_t time_ms);

/**
 * Convenience integration helper: searches active subtitle for current_pts in time_base,
 * and calls zstr_text_overlay_set_subtitle() on the provided overlay instance.
 */
int zstr_srt_parser_apply_to_overlay(const zstr_srt_parser_t *p,
                                     zstr_text_overlay_t *overlay,
                                     int64_t current_pts,
                                     AVRational time_base);

/**
 * Free SRT subtitle parser instance.
 */
void zstr_srt_parser_free(zstr_srt_parser_t **p);

#ifdef __cplusplus
}
#endif
