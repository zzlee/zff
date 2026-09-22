# zff (Z-FFmpeg Framework)

> **High-performance Pro-AV & Hardware Acceleration Extensions for FFmpeg**  
> Transplanted and evolved from `zstreamer`.

`zff` (pronounced */zɪf/*, stands for **Z-FFmpeg**) is a modern, modular multimedia extension ecosystem built directly on the industry-standard **FFmpeg** C architecture.

Instead of reinventing custom pipeline schedulers, pads, caps, and queues, `zff` adopts FFmpeg as its native pipeline backbone and provides:
1. **`libzff-core`**: A lightweight core bridge SDK for proprietary hardware structures (such as NVIDIA `NvBufSurface` on Jetson, Linux DMABUF, PTP hardware clocks, and custom SideData).
2. **`zstr_*` FFmpeg devices + C engines**: AVFormat/AVDevice devices (`libavformat`, `libavdevice`) plus C processing engines (`zff_engine.h` contract) implementing broadcast-grade protocols (SMPTE ST 2110-20/30/40, 2022-7 Hitless Merge), clock-drift compensated ASRC, and zero-copy hardware pipelines. No out-of-tree AVFilters (impossible with distro headers); no Dante yet (roadmap).

---

## Why zff? (The Evolution from zstreamer)

`zstreamer` successfully proved the algorithms for SMPTE ST 2110, WebRTC TWCC, Dante DEP, Jetson zero-copy, and ASRC drift compensation. However, maintaining a proprietary GStreamer-like pipeline engine imposed a steep learning curve and isolated the project from the broader multimedia community.

`zff` freezes the legacy `zstreamer` pipeline engine and transplants all core algorithms into native FFmpeg components:

| Feature | Legacy `zstreamer` | Modern `zff` |
| :--- | :--- | :--- |
| **Pipeline & Scheduler** | Custom `zst_pipeline`, `zst_scheduler`, `zst_pad` | **Standard FFmpeg `AVFilterGraph` & Threaded Queues** |
| **Buffer Model** | Custom `zst_buffer_t` with typed memory | **Native `AVFrame` & `AVPacket` (`AVBufferRef` lifecycle)** |
| **Caps Negotiation** | Custom caps intersection algorithms | **FFmpeg native `query_formats` & auto-filters** |
| **Plugin Namespace** | Custom `zst_element_register` | **All plugins prefixed with `zstr_xxxx`** |
| **Proprietary Hardware** | Custom allocators | **`libzff-core` zero-copy bridge (`AV_PIX_FMT_DRM_PRIME` / `NvBufSurface`)** |
| **Developer Experience** | Must learn custom APIs | **100% standard FFmpeg C API & `ffmpeg` CLI compatible** |

---

## Two-Tier Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                        Application / User Code                         │
│  (100% standard FFmpeg API: AVFormatContext, AVFilterGraph, AVFrame)   │
└───────────────────┬────────────────────────────────┬───────────────────┘
                    │ 1. Proprietary HW Bridge        │ 2. Standard FFmpeg pipeline
                    ▼                                ▼
┌──────────────────────────────────────┐  ┌──────────────────────────────┐
│          libzff-core.so              │  │      zstr_* FFmpeg Plugins   │
│   【 Hardware Bridge & Data SDK 】    │  │   【 Shared Object (.so) 】   │
├──────────────────────────────────────┤  ├──────────────────────────────┤
│ • NvBufSurface ↔ AVFrame Zero-copy   │  │ • C engines (no AVFilter):    │
│ • DMABUF / CUDA / OneAPI Memory      │  │   zstr_aresample, zstr_scale, │
│ • SideData FourCC & TLV Accessors    │  │   zstr_amix, zstr_rtp...      │
│ • IEEE 1588 PTP ↔ FFmpeg Timebase    │  │ • libavformat:               │
│ • Custom Extensible Struct Types     │  │   zstr_st2110, zstr_webrtc,  │
│                                      │  │   zstr_rtspserver...          │
│                                      │  │ • libavdevice:               │
│                                      │  │   zstr_v4l2, zstr_alsa...    │
└──────────────────────────────────────┘  └──────────────────────────────┘
                    ▲                                │
                    └────────────────────────────────┘
                      Plugins internally use Core for HW translation
```

---

## Quick Example

### 1. Using standard FFmpeg CLI (devices only):
```bash
# Capture camera + mic, stream raw packets to ST 2110 (processing engines
# are C API, not -vf/-af: out-of-tree AVFilters are impossible with
# distro libav* headers, see llms.txt)
ffmpeg \
  -f zstr_v4l2 -video_size 1280x720 -i dummy \
  -f zstr_alsa -i dummy \
  -f zstr_st2110_mux -host 239.1.1.1 -port 20000 dummy
```

### 2. Using standard FFmpeg C API with `libzff-core`:
```c
#include <zff/zff_core.h>
#include <zff/zff_hw.h>
#include <zff/plugins/zstr_scale.h>
#include <libavformat/avformat.h>

int main() {
    // 1. Initialize zff plugins into FFmpeg
    zff_plugins_register_all();

    // 2. Wrap proprietary Jetson NvBufSurface into standard AVFrame (Zero-copy!)
    NvBufSurface *surf = get_jetson_camera_surface();
    AVFrame *frame = zff_nvbuf_wrap_frame(surf, 1920, 1080);

    // 3. Attach IEEE 1588 PTP hardware timestamp via standard SideData
    zff_ptp_time_t ptp = { .tai_nanoseconds = 1718000000000ULL, .domain = 0 };
    zff_frame_set_ptp(frame, &ptp);

    // 4. Run a C engine directly (no filter graph needed out-of-tree)
    zstr_scale_t *sc = zstr_scale_alloc("w=1920:h=1080");
    AVFrame *out = av_frame_alloc();   // engine sizes + allocates buffers
    zstr_scale_process(sc, frame, out);
    zstr_scale_free(&sc);
    av_frame_free(&frame);
    av_frame_free(&out);
}
```

---

## Documentation Links

- [Architecture & Design Details](ARCHITECTURE.md) — Buffer models, zero-copy, SideData, and dataflow.
- [Complete Elements Inventory](ELEMENTS.md) — All 101+ elements mapped to `zstr_xxxx` plugins.
- [Roadmap & Migration Plan](ROADMAP.md) — Step-by-step engineering schedule.
- [Kaggle CUDA Runbook](docs/KAGGLE_CUDA.md) — free-GPU video codec testing.
