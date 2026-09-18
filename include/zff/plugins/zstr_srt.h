/*=============================================================================
    zstr_srt.h — SRT (Secure Reliable Transport) Streaming & Subtitle Parser Suite
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
#include "zff/internal/zff_ffformat.h"
#include "zff/plugins/zstr_text_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * SRT Subtitle Parser API
 * --------------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------------
 * SRT Network Streaming Transport API
 * --------------------------------------------------------------------------- */
typedef enum {
    ZSTR_SRT_MODE_CALLER = 0,    /**< Connect to remote listener */
    ZSTR_SRT_MODE_LISTENER = 1,  /**< Bind and accept incoming connection */
    ZSTR_SRT_MODE_RENDEZVOUS = 2 /**< P2P Rendezvous connection */
} zstr_srt_mode_t;

typedef struct {
    zstr_srt_mode_t mode;
    const char *host;            /**< Destination IP for caller, or bind IP for listener */
    uint16_t port;               /**< UDP port */
    int latency_ms;              /**< Receiver/Sender latency in ms (default: 120ms) */
    const char *passphrase;      /**< AES encryption passphrase (NULL = disabled) */
    int pbkeylen;                /**< Passphrase key length: 16, 24, 32 bytes (default: 16) */
    const char *streamid;        /**< SRT stream id / resource string */
    int payload_size;            /**< Max SRT payload size in bytes (default: 1316) */
    int timeout_ms;              /**< Connect/read/write timeout in ms (default: 3000ms) */
} zstr_srt_config_t;

typedef struct zstr_srt_source zstr_srt_source_t;
typedef struct zstr_srt_sink zstr_srt_sink_t;

/**
 * Parse an SRT URL (e.g. "srt://127.0.0.1:9000?mode=caller&latency=200&passphrase=abc")
 */
int zstr_srt_parse_url(const char *url, zstr_srt_config_t *cfg,
                       char *host_buf, size_t host_buf_size,
                       char *pass_buf, size_t pass_buf_size,
                       char *streamid_buf, size_t streamid_buf_size);

/* --- SRT Source (Receiver) --- */
zstr_srt_source_t* zstr_srt_source_create(const zstr_srt_config_t *cfg);
zstr_srt_source_t* zstr_srt_source_create_from_url(const char *url);
int zstr_srt_source_read(zstr_srt_source_t *s, uint8_t *buf, int size);
int zstr_srt_source_read_packet(zstr_srt_source_t *s, AVPacket *pkt);
void zstr_srt_source_close(zstr_srt_source_t **s);

/* --- SRT Sink (Sender) --- */
zstr_srt_sink_t* zstr_srt_sink_create(const zstr_srt_config_t *cfg);
zstr_srt_sink_t* zstr_srt_sink_create_from_url(const char *url);
int zstr_srt_sink_write(zstr_srt_sink_t *s, const uint8_t *buf, int size);
int zstr_srt_sink_write_packet(zstr_srt_sink_t *s, const AVPacket *pkt);
void zstr_srt_sink_close(zstr_srt_sink_t **s);

/* ---------------------------------------------------------------------------
 * FFmpeg AVInputFormat & AVOutputFormat Declarations
 * --------------------------------------------------------------------------- */
extern const AVInputFormat ff_zstr_srt_source_demuxer;
extern const FFOutputFormat ff_zstr_srt_sink_muxer;

#ifdef __cplusplus
}
#endif
