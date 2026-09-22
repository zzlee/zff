/*=============================================================================
    zff_engine.h — Unified Filter Engine Contract (zff)

    Every zff processing engine follows this contract so that all engines
    are constructed, driven, and torn down the same way, whether they are
    frame engines (AVFrame in/out), packet engines (AVPacket in/out), or
    converters. Engines are deliberately NOT AVFilters: out-of-tree
    AVFilter/AVBitStreamFilter implementation is impossible with distro
    libav* headers (opaque AVFilterPad, no formats constructors, no
    filter() callback) — see llms.txt. This contract is the out-of-tree
    ceiling, kept compatible with a future in-tree FFmpeg port.

    1. LIFECYCLE
       alloc/create -> process* -> flush (if stateful) -> free(&ptr).
       free() takes a pointer-to-pointer, NULLs it, and tolerates NULL.

    2. CONSTRUCTION (two families, chosen by parameter shape)
       - alloc(opt_string): engines configured by key=value pairs, e.g.
           zstr_aresample_alloc("out_sample_rate=48000:asrc_mode=pts").
         opt_string may be NULL (= defaults).
       - create(cfg): converters taking a typed C struct, e.g.
           zstr_rtp_payloader_create(ZSTR_RTP_CODEC_H264, 96, ...).
         Both return NULL on failure (no partial objects, no errno codes).

    3. OPTION SYNTAX
       The string form is always "k=v:k2=v2" (',' also accepted). Engines
       SHOULD use libavutil AVOption; engines with indexed/per-item syntax
       (e.g. amix "volume@0=0.5") may hand-parse but MUST keep the syntax.

    4. PROCESS RULES
       - Never consume, free, or modify the input object.
       - Frame engines: the caller passes an empty output AVFrame; the
         engine sizes it and allocates its buffers (av_frame_get_buffer).
         Exception: verified zero-copy passthrough may av_frame_ref.
       - Packet engines that fan out (payloaders, mux-duplicators):
           process(s, in, &array_out, &nb_out); caller frees via the
         family free_packets() helper.
       - Packet engines that reassemble (depayloders):
           process(s, in, caller_out, &ready); ready=false means "feed me
         more", and caller_out is untouched in that case.
       - One-shot converters (encode/decode): process-style verbs with
         explicit input/output ownership in their doc comments.

    5. ERRORS: 0 on success, negative AVERROR on failure. No custom codes.

    6. FLUSH: stateful engines (delay lines, e.g. resamplers) MUST provide
       flush(); stateless engines omit it.

    7. SET_PARAM (optional): engines that support runtime reconfiguration
       declare set_param(s, "k=v:..."); a successful call takes effect on
       the NEXT process() call at the latest. Engines without it are
       configured once at alloc/create.

    8. THREADING: an instance is driven by one thread at a time; concurrent
       process() on the same instance is undefined unless documented.

    9. TIMESTAMPS: each engine documents its PTS convention in its header
       (stream time_base vs nanoseconds); the contract requires the
       documentation, not a single convention.
 =============================================================================*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Engine contract version (bump on incompatible contract change). */
#define ZSTR_ENGINE_CONTRACT_VERSION 1

#ifdef __cplusplus
}
#endif
