/*=============================================================================
    zstr_srt_common.h — Shared helpers and URI parsing for SRT streaming
=============================================================================*/
#pragma once

#include "zff/plugins/zstr_srt.h"
#include <srt/srt.h>
#include <pthread.h>

void zstr_srt_global_init(void);
void zstr_srt_global_cleanup(void);

int zstr_srt_apply_socket_options(SRTSOCKET sock, const zstr_srt_config_t *cfg, bool is_sender);
