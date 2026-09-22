# zff Complete Elements & Plugins Inventory

All elements migrated from `zstreamer` are named with the **`zstr_`** prefix and registered in standard FFmpeg subsystems.

---

## 1. Devices (`libavdevice`)

| Plugin Name | Type | Original Element | Transplanted Core Algorithm / Feature | Buffer Model | Options (`AVOption`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`zstr_v4l2`** | indev | `v4l2_source.c` | **VIDIOC_EXPBUF DMABUF export**; synthetic mock fallback for headless CI | Outputs `AVFrame` (`AV_PIX_FMT_DRM_PRIME`) | `device`, `video_size`, `pixel_format`, `memory_type`, `is_mock` |
| **`zstr_v4l2_sink`** | outdev | `v4l2_sink.c` | **DMABUF import write**; V4L2 output loopback buffer pacing | Takes `AVFrame` (DRM_PRIME or CPU YUV) | `device`, `pix_fmt` |
| **`zstr_alsa`** | indev | `alsa_source.c` | **RingBuffer underrun/overrun protection**; non-blocking poll; mock sine fallback | Outputs `AVFrame` (`S16LE` / `F32LE`) | `device`, `sample_rate`, `channels`, `block_size`, `mock_fallback` |
| **`zstr_alsa_sink`** | outdev | `alsa_sink.c` | **Hardware PTS latency compensation**; prevents audio underrun pop | Takes audio `AVFrame` | `device`, `buffer_time`, `period_time` |
| **`zstr_sc6f0`** | indev | `sc6f0_source.c` | **SC6f0 FPGA DMA capture**; direct physical memory alignment | Outputs aligned hardware `AVFrame` | `channel`, `resolution`, `fps` |

---

## 2. Audio Processing Filters (`libavfilter`)

| Plugin Name | Type | Original Element | Transplanted Core Algorithm / Feature | Buffer Model | Options (`AVOption`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`zstr_aresample`** | Audio engine (C, `alloc/process/flush/free`) | `audio_resampler.c` | **PTS-based ASRC drift compensation**; dynamic `swr_set_compensation`; fractional rate override (`rate_numer/denom`); block sample slicing | `AVFrame` ➔ `AVFrame` | `sample_rate`, `max_drift_ppm`, `drift_interval`, `rate_numer`, `rate_denom`, `block_samples` |
| **`zstr_amix`** | Audio Filter (`af_`) | `audio_mixer.c` | **Dynamic multi-channel mixer**; per-channel volume/mute; soft-clipping float limiter | Multiple `AVFrame` ➔ Single `AVFrame` | `inputs`, `weights`, `normalize`, `dropout_transition` |
| **`zstr_audiotestsrc`** | Audio Source (indev) | `audio_test_src.c` | **Multi-waveform generator**; high-precision timestamp generation (Sine, Noise, DTMF) | Generates `AVFrame` | `sample_rate`, `channels`, `freq`, `wave_type`, `duration` |

---

## 3. Video Processing & Display Filters (`libavfilter`)

| Plugin Name | Type | Original Element | Transplanted Core Algorithm / Feature | Buffer Model | Options (`AVOption`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`zstr_scale`** | Video Filter (`vf_`) | `video_scaler.c` | **64-byte aligned SIMD scaling**; fast stride conversions; AVX2/AVX-512 optimization | `AVFrame` ➔ `AVFrame` | `w`, `h`, `format`, `flags`, `align` |
| **`zstr_text_overlay`**| Video Filter (`vf_`) | `text_overlay.c` | **Lightweight OSD overlay**; PTS/timecode macros; bitmap glyph cache | In-place or copy `AVFrame` | `text`, `x`, `y`, `font_size`, `color`, `timecode_mode` |
| **`zstr_vtestsrc`** | Video Source (`vsrc_`) | `video_test_src.c` | **SMPTE-170M / EBU colorbars**; bouncing ball; zoneplate generator | Generates raw `AVFrame` | `size`, `rate`, `pattern` |
| **`zstr_glsink`** | Video Sink (`vsink_`) | `gl_sink.c` | **OpenGL/GLX zero-copy display**; GLSL YUV420P/NV12 ➔ RGB shader; Xvfb headless support | Takes `AVFrame` (CPU or DRM_PRIME) | `display`, `fullscreen`, `vsync`, `window_title` |
| **`zstr_glcompositor`**| Video Filter (`vf_`) | `gl_comp_sink.c` | **GPU multi-channel compositor**; PiP, grid layouts, custom borders, alpha blending | Multi `AVFrame` ➔ Single `AVFrame` | `layout`, `background`, `border_width`, `border_color` |
| **`zstr_ipp_comp`** | Video Filter (`vf_`) | `ipp_comp_sink.c` | **Intel IPP CPU compositor**; high-speed CPU multi-video mixing | Multi `AVFrame` ➔ Single `AVFrame` | `layout`, `threads` |
| **`zstr_x11sink`** | Video Sink (`vsink_`) | `x11_sink.c` | **X11 software display**; sws straight into XImage (TrueColor fast path, per-pixel fallback for exotic visuals); Expose re-blit; Xvfb-tested | Takes `AVFrame` | `display`, `window_title`, `is_mock` |

---

## 4. Broadcast SMPTE ST 2110 & Dante Protocols (`libavformat`)

| Plugin Name | Type | Original Element | Transplanted Core Algorithm / Feature | Buffer Model | Options (`AVOption`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`zstr_st2110_demux`** | Demuxer | `st2110_20/30/40_depayloader.c` | **ST 2110 receiver**; raw UDP socket; use `zstr_st2110_sdp_parse` for session setup | UDP ➔ `AVPacket` | `host`, `port` |
| **`zstr_st2110_mux`** | Muxer | `st2110_20/30/40_payloader.c` | **ST 2110 sender**; raw UDP socket; pair with `zstr_st2110_21` pacer + `zstr_st2110_sdp_generate` | `AVPacket` ➔ UDP | `host`, `port`, `pt` |
| **`zstr_st2110_sdp`** | Library | `sdp_muxer.c` (st2110 mode) + `sdp_demuxer.c` (line parser) | **ST 2110 SDP generate + parse**; raw/90000 fmtp sampling, L16/L24 + mediaclk, ts-refclk/keywait | SDP text ↔ struct | — (C API) |
| **`zstr_st2110_21`** | Filter / Pacer | `st2110_21_payloader.c` | **ST 2110-21 traffic shaper**; Narrow (N), Narrow Linear (NL), Wide (W) sender pacing | Intercepts `AVPacket` | `pacer_type`, `c_max`, `vrx_full` |
| **`zstr_st2110_22`** | Mux / Demux | `st2110_22_payloader.c` | **JPEG-XS constant bitrate streaming**; slice-based packetization | `AVPacket` | `bitrate`, `slice_mode` |
| **`zstr_st2022_7`** | URLProtocol / Demux | `st2110_redundancy_mux/demux.c` | **SMPTE 2022-7 hitless**; mux clones bit-identical A/B copies, demux drops late duplicates | `AVPacket` ↔ dual path | — (C API) |
| **`zstr_st2022_5_fec`**| Filter / Protocol | `st2110_fec.c` | **SMPTE 2022-5 1D/2D XOR Forward Error Correction** matrix | `AVPacket` | `fec_l`, `fec_d`, `fec_mode` |
| **`zstr_dante_demux`** | Demuxer | `dante_dep_source.c`, `dep_audio.c` | **Dante / DEP multicast audio receiver**; 24-bit/32-bit float PCM depayload | UDP ➔ `AVFrame` | `session_name`, `channels`, `multicast_ip`, `port`, `latency_us` |
| **`zstr_dante_mux`** | Muxer | `dante_dep_sink.c`, `dante_udp_sink.c` | **Dante / DEP audio sender**; high-precision timeslot RTP packaging | `AVFrame` ➔ UDP `AVPacket` | `dest_ip`, `port`, `channels`, `flow_id`, `pace-packets` |
| **`zstr_dante_coord`** | Time Utility | `dante_video_coordinator.c` | **Dante AV lip-sync coordinator**; PTP-locked audio/video phase alignment | Time alignment control | `ptp_clock_id`, `sync_tolerance_us` |

---

## 5. Streaming & WebRTC Protocols (`libavformat`)

| Plugin Name | Type | Original Element | Transplanted Core Algorithm / Feature | Buffer Model | Options (`AVOption`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`zstr_webrtc`** | Protocol / Mux / Demux | `webrtc_endpoint.c`, `zst_webrtc_twcc.c` | **WebRTC + GCC / TWCC congestion control**; RFC 8888 CCFB; AIMD delay/loss bandwidth estimation | Bidirectional `AVPacket` | `signaling_url`, `stun_server`, `turn_server`, `twcc_enable`, `min_bitrate`, `max_bitrate` |
| **`zstr_rtspserver`** | Muxer / Service | `rtsp_server.c` | **Multi-session RTSP daemon**; TCP interleaved and UDP unicast streaming | Takes `AVPacket` | `port`, `mount_point`, `transport`, `max_clients` |
| **`zstr_net`** | URLProtocol | `net_source.c`, `net_sink.c` | **High-throughput raw TCP/UDP** with socket buffer auto-tuning | Byte-stream `AVIOContext` | `buffer_size`, `timeout`, `reuse_addr` |
| **`zstr_hlssink`** | Muxer | `hls_sink.c`, `http_server.c` | **Embedded HLS + micro HTTP server**; in-memory segment caching | Takes `AVPacket` | `hls_time`, `hls_list_size`, `http_port`, `in_memory_segments` |

---

## 6. Hardware Codecs & Accelerators (`libavcodec` & `libavfilter`)

| Plugin Name | Type | Original Element | Transplanted Core Algorithm / Feature | Buffer Model | Options (`AVOption`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`zstr_nvenc` / `zstr_nvdec`** | Encoder / Decoder | `nv_video_encoder.c`, `nv_video_decoder.c` | **Direct CUDA pointer mapping**; Jetson VIC integration | `AV_PIX_FMT_CUDA` / `DRM_PRIME` | `gpu`, `preset`, `rc`, `bitrate` |
| **`zstr_nvscale`** | Video Filter | `nv_video_scaler.c` | **NPP GPU image scaling & format conversion** | `AV_PIX_FMT_CUDA` | `w`, `h`, `interp_algo` |
| **`zstr_oneapi`** | Encoder / Decoder | `oneapi_video_encoder.c`, `oneapi_video_decoder.c` | **Intel OneVPL hardware codec acceleration** | `AV_PIX_FMT_QSV` / `VAAPI` | `device_id`, `low_power`, `quality` |
