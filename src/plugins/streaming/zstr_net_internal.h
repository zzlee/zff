/*=============================================================================
    zstr_net_internal.h — Internal socket transport helpers for plugins
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>

typedef enum {
    ZSTR_NET_PROTO_UDP,
    ZSTR_NET_PROTO_TCP_CLIENT,
    ZSTR_NET_PROTO_TCP_SERVER
} zstr_net_protocol_t;

typedef struct {
    zstr_net_protocol_t protocol;
    const char *host;
    uint16_t port;
    int buffer_size;
    int timeout_ms;
    int ttl;
    bool loopback;
    bool is_multicast;
} zstr_net_config_t;

typedef struct zstr_net_source zstr_net_source_t;
typedef struct zstr_net_sink zstr_net_sink_t;

zstr_net_source_t* zstr_net_source_create(const zstr_net_config_t *cfg);
int zstr_net_source_read(zstr_net_source_t *s, uint8_t *buf, int size);
int zstr_net_source_read_packet(zstr_net_source_t *s, AVPacket *pkt);
void zstr_net_source_close(zstr_net_source_t **s);

zstr_net_sink_t* zstr_net_sink_create(const zstr_net_config_t *cfg);
int zstr_net_sink_write(zstr_net_sink_t *s, const uint8_t *buf, int size);
int zstr_net_sink_write_packet(zstr_net_sink_t *s, const AVPacket *pkt);
void zstr_net_sink_close(zstr_net_sink_t **s);
