# zff Engineering Roadmap & Migration Plan

This document outlines the phased migration plan to freeze `zstreamer` and build `zff`.

---

## Phase 1: `libzff-core` SDK (Foundation)
**Duration:** 2 Weeks  
**Deliverables:**
1. **Public Headers**:
   - `<zff/zff_core.h>`: Common definitions, registration APIs.
   - `<zff/zff_hw.h>`: Hardware zero-copy wrappers (`zff_nvbuf_wrap_frame`, `zff_nvbuf_unwrap_frame`, `zff_drm_wrap_frame`).
   - `<zff/zff_sidedata.h>`: FourCC definitions (`ZSTR_TAG_NVBUF`, `ZSTR_TAG_PTP`), TLV bundle accessors.
   - `<zff/zff_time.h>`: PTP / TAI ↔ FFmpeg `AVRational` conversion.
2. **Implementation**:
   - `src/core/zff_hw_nvbuf.c`
   - `src/core/zff_sidedata.c`
   - `src/core/zff_time.c`
3. **Unit Tests**:
   - `tests/test_nvbuf_bridge.c`: Lifecycle, refcounting, and zero-leak validation.
   - `tests/test_sidedata.c`: Multi-tag insertion and extraction.

---

## Phase 2: Essential Devices & Audio/Video Filters
**Duration:** 3 Weeks  
**Deliverables:**
1. **`libzstr_devices.so`**:
   - `zstr_v4l2` (indev): Ported from `v4l2_source.c` (VIDIOC_EXPBUF DMABUF export + mock fallback).
   - `zstr_alsa` (indev): Ported from `alsa_source.c` (RingBuffer protection + non-blocking poll).
2. **`libzstr_audio.so`**:
   - `zstr_aresample` (C engine, not `af_`): Ported from `audio_resampler.c` (PTS drift tracking + `swr_set_compensation`).
   - `zstr_amix` (C engine, not `af_`): Ported from `audio_mixer.c`.
3. **`libzstr_video.so`**:
   - `zstr_scale` (C engine): 64-byte aligned SIMD scaling.
   - `zstr_text_overlay` (C engine): OSD timestamp overlay.
   - `zstr_glsink` (outdev): OpenGL/GLX zero-copy display sink with Xvfb CI support.

---

## Phase 3: Broadcast Protocols (ST 2110 & Dante)
**Duration:** 4 Weeks  
**Deliverables:**
1. **`libzstr_broadcast.so`**:
   - `zstr_st2110_mux` / `zstr_st2110_demux`: SMPTE ST 2110-20/30/40 RFC 4175 packetization.
   - `zstr_st2110_21`: Narrow / Wide sender traffic shaping pacer.
   - `zstr_st2022_7`: Dual-network Hitless Merge deduplicator.
   - `zstr_st2022_5_fec`: 1D/2D XOR Forward Error Correction.
   - `zstr_dante_mux` / `zstr_dante_demux`: Dante DEP RTP packetizer/depacketizer.
   - `zstr_dante_coord`: PTP-aligned audio/video synchronization coordinator.

---

## Phase 4: Real-time Streaming (WebRTC & RTSP Server)
**Duration:** 3 Weeks  
**Deliverables:**
1. **`libzstr_streaming.so`**:
   - `zstr_webrtc`: `libdatachannel` integration with GCC/TWCC RFC 8888 feedback and bandwidth adaptation.
   - `zstr_rtspserver`: Multi-client RTSP server daemon supporting TCP interleaved and UDP unicast.
   - `zstr_hlssink`: In-memory HLS segmenter with micro HTTP server.

---

## Phase 5: Hardware Codecs & Packaging
**Duration:** 2 Weeks  
**Deliverables:**
1. **Hardware Codecs**:
   - `zstr_nvenc` / `zstr_nvdec` / `zstr_nvscale` (NVIDIA CUDA / Jetson VIC).
   - `zstr_sc6f0` (SC6f0 FPGA ARM64 capture).
2. **Release Packaging**:
   - Debian `.deb` packages for x86_64 and SC6f0 ARM64 (`xlnk2_arm64`).
   - Unified Docker images for validation and regression tests.
