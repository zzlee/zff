/*=============================================================================
    zstr_srt_common.c — SRT Common utilities and configuration parser
=============================================================================*/
#include "zstr_srt_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/error.h>

static pthread_mutex_t s_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_init_count = 0;

void zstr_srt_global_init(void)
{
    pthread_mutex_lock(&s_init_lock);
    if (s_init_count == 0) {
        srt_startup();
    }
    s_init_count++;
    pthread_mutex_unlock(&s_init_lock);
}

void zstr_srt_global_cleanup(void)
{
    pthread_mutex_lock(&s_init_lock);
    if (s_init_count > 0) {
        s_init_count--;
    }
    pthread_mutex_unlock(&s_init_lock);
}

int zstr_srt_apply_socket_options(SRTSOCKET sock, const zstr_srt_config_t *cfg, bool is_sender)
{
    if (!cfg || sock == SRT_INVALID_SOCK) return AVERROR(EINVAL);

    int trans_type = SRTT_LIVE;
    srt_setsockopt(sock, 0, SRTO_TRANSTYPE, &trans_type, sizeof(trans_type));

    if (cfg->latency_ms > 0) {
        int latency = cfg->latency_ms;
        srt_setsockopt(sock, 0, SRTO_LATENCY, &latency, sizeof(latency));
    }

    if (cfg->payload_size > 0) {
        int payload_size = cfg->payload_size;
        srt_setsockopt(sock, 0, SRTO_PAYLOADSIZE, &payload_size, sizeof(payload_size));
    }

    if (cfg->passphrase && cfg->passphrase[0]) {
        srt_setsockopt(sock, 0, SRTO_PASSPHRASE, cfg->passphrase, (int)strlen(cfg->passphrase));
        if (cfg->pbkeylen == 16 || cfg->pbkeylen == 24 || cfg->pbkeylen == 32) {
            int keylen = cfg->pbkeylen;
            srt_setsockopt(sock, 0, SRTO_PBKEYLEN, &keylen, sizeof(keylen));
        }
    }

    if (cfg->streamid && cfg->streamid[0]) {
        srt_setsockopt(sock, 0, SRTO_STREAMID, cfg->streamid, (int)strlen(cfg->streamid));
    }

    int timeout = cfg->timeout_ms > 0 ? cfg->timeout_ms : 3000;
    if (is_sender) {
        srt_setsockopt(sock, 0, SRTO_SNDTIMEO, &timeout, sizeof(timeout));
    } else {
        srt_setsockopt(sock, 0, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    return 0;
}

int zstr_srt_parse_url(const char *url, zstr_srt_config_t *cfg,
                       char *host_buf, size_t host_buf_size,
                       char *pass_buf, size_t pass_buf_size,
                       char *streamid_buf, size_t streamid_buf_size)
{
    if (!url || !cfg) return AVERROR(EINVAL);

    /* Defaults */
    cfg->mode = ZSTR_SRT_MODE_CALLER;
    cfg->latency_ms = 120;
    cfg->payload_size = 1316;
    cfg->timeout_ms = 3000;
    cfg->pbkeylen = 16;
    cfg->passphrase = NULL;
    cfg->streamid = NULL;
    cfg->host = "127.0.0.1";
    cfg->port = 9000;

    const char *p = url;
    if (strncmp(p, "srt://", 6) == 0) {
        p += 6;
    }

    const char *colon = strchr(p, ':');
    const char *qmark = strchr(p, '?');

    if (colon && (!qmark || colon < qmark)) {
        size_t hlen = colon - p;
        if (host_buf && host_buf_size > 0) {
            if (hlen >= host_buf_size) hlen = host_buf_size - 1;
            strncpy(host_buf, p, hlen);
            host_buf[hlen] = '\0';
            cfg->host = host_buf;
        }
        cfg->port = (uint16_t)atoi(colon + 1);
    } else if (qmark) {
        size_t hlen = qmark - p;
        if (host_buf && host_buf_size > 0) {
            if (hlen >= host_buf_size) hlen = host_buf_size - 1;
            strncpy(host_buf, p, hlen);
            host_buf[hlen] = '\0';
            cfg->host = host_buf;
        }
    } else if (*p) {
        if (host_buf && host_buf_size > 0) {
            strncpy(host_buf, p, host_buf_size - 1);
            host_buf[host_buf_size - 1] = '\0';
            cfg->host = host_buf;
        }
    }

    if (qmark) {
        const char *query = qmark + 1;
        char qcopy[512];
        strncpy(qcopy, query, sizeof(qcopy) - 1);
        qcopy[sizeof(qcopy) - 1] = '\0';

        char *token = strtok(qcopy, "&;");
        while (token) {
            char *eq = strchr(token, '=');
            if (eq) {
                *eq = '\0';
                const char *k = token;
                const char *v = eq + 1;

                if (strcmp(k, "mode") == 0) {
                    if (strcmp(v, "listener") == 0) cfg->mode = ZSTR_SRT_MODE_LISTENER;
                    else if (strcmp(v, "rendezvous") == 0) cfg->mode = ZSTR_SRT_MODE_RENDEZVOUS;
                    else cfg->mode = ZSTR_SRT_MODE_CALLER;
                } else if (strcmp(k, "latency") == 0) {
                    cfg->latency_ms = atoi(v);
                } else if (strcmp(k, "payload_size") == 0 || strcmp(k, "pkt_size") == 0) {
                    cfg->payload_size = atoi(v);
                } else if (strcmp(k, "timeout") == 0 || strcmp(k, "timeout_ms") == 0) {
                    cfg->timeout_ms = atoi(v);
                } else if (strcmp(k, "passphrase") == 0) {
                    if (pass_buf && pass_buf_size > 0) {
                        strncpy(pass_buf, v, pass_buf_size - 1);
                        pass_buf[pass_buf_size - 1] = '\0';
                        cfg->passphrase = pass_buf;
                    }
                } else if (strcmp(k, "pbkeylen") == 0) {
                    cfg->pbkeylen = atoi(v);
                } else if (strcmp(k, "streamid") == 0) {
                    if (streamid_buf && streamid_buf_size > 0) {
                        strncpy(streamid_buf, v, streamid_buf_size - 1);
                        streamid_buf[streamid_buf_size - 1] = '\0';
                        cfg->streamid = streamid_buf;
                    }
                }
            }
            token = strtok(NULL, "&;");
        }
    }

    return 0;
}
