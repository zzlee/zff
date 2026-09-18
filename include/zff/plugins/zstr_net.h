/*=============================================================================
    zstr_net.h — Network UDP/TCP Source and Sink Devices (AVInputFormat / AVOutputFormat)
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include "zff/internal/zff_ffformat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZSTR_NET_PROTO_UDP,
    ZSTR_NET_PROTO_TCP_CLIENT,
    ZSTR_NET_PROTO_TCP_SERVER
} zstr_net_protocol_t;

typedef struct {
    zstr_net_protocol_t protocol;
    const char *host;         /**< Host or IP address to connect or bind to (e.g. "127.0.0.1" or "0.0.0.0") */
    uint16_t port;            /**< Port number */
    int buffer_size;          /**< Socket receive/send buffer or packet buffer size (default: 65536) */
    int timeout_ms;           /**< Socket read/write timeout in ms (0 = non-blocking or default) */
    int ttl;                  /**< Multicast TTL (for UDP) */
    bool loopback;            /**< Multicast loopback enable */
    bool is_multicast;        /**< Multicast group join enable */
} zstr_net_config_t;

typedef struct zstr_net_source zstr_net_source_t;
typedef struct zstr_net_sink zstr_net_sink_t;

/* ---------------------------------------------------------------------------
 * C Direct API: Network Source (Receiver)
 * --------------------------------------------------------------------------- */
zstr_net_source_t* zstr_net_source_create(const zstr_net_config_t *cfg);
int zstr_net_source_read(zstr_net_source_t *s, uint8_t *buf, int size);
int zstr_net_source_read_packet(zstr_net_source_t *s, AVPacket *pkt);
void zstr_net_source_close(zstr_net_source_t **s);

/* ---------------------------------------------------------------------------
 * C Direct API: Network Sink (Sender)
 * --------------------------------------------------------------------------- */
zstr_net_sink_t* zstr_net_sink_create(const zstr_net_config_t *cfg);
int zstr_net_sink_write(zstr_net_sink_t *s, const uint8_t *buf, int size);
int zstr_net_sink_write_packet(zstr_net_sink_t *s, const AVPacket *pkt);
void zstr_net_sink_close(zstr_net_sink_t **s);

/* ---------------------------------------------------------------------------
 * FFmpeg AVInputFormat & AVOutputFormat Declarations
 * --------------------------------------------------------------------------- */
extern const AVInputFormat ff_zstr_net_source_demuxer;
extern const FFOutputFormat ff_zstr_net_sink_muxer;

#ifdef __cplusplus
}
#endif
