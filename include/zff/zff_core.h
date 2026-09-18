/*=============================================================================
    zff_core.h — zff Core Bridge SDK Public Definitions
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <libavutil/frame.h>
#include <libavcodec/packet.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Custom FourCC SideData Identifiers ──────────────────────────────────── */
#define ZSTR_TAG_NVBUF ((enum AVFrameSideDataType)MKTAG('N', 'V', 'B', 'F'))
#define ZSTR_TAG_PTP   ((enum AVFrameSideDataType)MKTAG('P', 'T', 'P', 'T'))
#define ZSTR_TAG_BBOX  ((enum AVFrameSideDataType)MKTAG('B', 'B', 'O', 'X'))
#define ZSTR_TAG_USER  ((enum AVFrameSideDataType)MKTAG('Z', 'U', 'S', 'R'))

/* ── Plugin Global Registration ─────────────────────────────────────────── */

/**
 * Register all zff native FFmpeg plugins (AVFilter, AVFormat, AVDevice).
 * Call this once during application initialization.
 */
int zff_plugins_register_all(void);

#ifdef __cplusplus
}
#endif
