/*=============================================================================
    zstr_srt_common.h — Shared internal definitions and URI parsing for SRT streaming
=============================================================================*/
#pragma once

#include "zff/plugins/zstr_srt.h"
#include <srt/srt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/packet.h>

typedef enum {
    ZSTR_SRT_MODE_CALLER = 0,
    ZSTR_SRT_MODE_LISTENER = 1,
    ZSTR_SRT_MODE_RENDEZVOUS = 2
} zstr_srt_mode_t;

typedef struct {
    zstr_srt_mode_t mode;
    const char *host;
    uint16_t port;
    int latency_ms;
    const char *passphrase;
    int pbkeylen;
    const char *streamid;
    int payload_size;
    int timeout_ms;
} zstr_srt_config_t;

typedef struct zstr_srt_source zstr_srt_source_t;
typedef struct zstr_srt_sink zstr_srt_sink_t;

void zstr_srt_global_init(void);
void zstr_srt_global_cleanup(void);

int zstr_srt_parse_url(const char *url, zstr_srt_config_t *cfg,
                       char *host_buf, size_t host_buf_size,
                       char *pass_buf, size_t pass_buf_size,
                       char *streamid_buf, size_t streamid_buf_size);

int zstr_srt_apply_socket_options(SRTSOCKET sock, const zstr_srt_config_t *cfg, bool is_sender);

/* Internal socket source routines */
zstr_srt_source_t* zstr_srt_source_create(const zstr_srt_config_t *cfg);
int zstr_srt_source_read(zstr_srt_source_t *s, uint8_t *buf, int size);
int zstr_srt_source_read_packet(zstr_srt_source_t *s, AVPacket *pkt);
void zstr_srt_source_close(zstr_srt_source_t **s);

/* Internal socket sink routines */
zstr_srt_sink_t* zstr_srt_sink_create(const zstr_srt_config_t *cfg);
int zstr_srt_sink_write(zstr_srt_sink_t *s, const uint8_t *buf, int size);
int zstr_srt_sink_write_packet(zstr_srt_sink_t *s, const AVPacket *pkt);
void zstr_srt_sink_close(zstr_srt_sink_t **s);
