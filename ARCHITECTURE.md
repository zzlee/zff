# zff Architecture & Technical Specification

This document details the architectural foundation of `zff`, specifically focusing on **memory lifecycle, hardware zero-copy bridges, SideData multiplexing, and scheduling**.

---

## 1. Unified Buffer Architecture: `AVFrame` & `AVPacket`

In `zff`, all proprietary buffer structures (like legacy `zst_buffer_t`) are replaced by standard FFmpeg structures governed by atomic reference counting (`AVBufferRef`):

```
                        ┌──────────────────────────────────────────────┐
                        │                AVBufferRef                   │
                        │  (Atomic refcount + data pointer + custom cb) │
                        └──────────────────────┬───────────────────────┘
                                               │ Manages
                              ┌────────────────┴────────────────┐
                              ▼                                 ▼
                       【 AVFrame 】                     【 AVPacket 】
                     (Raw Audio / Video)             (Compressed Streams / RTP)
                  - data[8], linesize[8]              - data, size
                  - width, height, format             - pts, dts, duration
                  - pts, nb_samples                   - flags (AV_PKT_FLAG_KEY)
                  - hw_frames_ctx (DRM/CUDA)          - side_data[]
                  - side_data[]
```

### Allocation Rules:
1. **Inside `AVFilter`s**: Video frames must be allocated via `ff_get_video_buffer(outlink, w, h)`, and audio frames via `ff_get_audio_buffer(outlink, nb_samples)`. This allows FFmpeg's internal buffer pool to recycle memory with zero allocation overhead in the hot loop.
2. **Inside Custom Demuxers/Muxers**: Use `av_packet_alloc()` / `av_packet_free()`.

### Engine Contract (out-of-tree ceiling)

Out-of-tree `AVFilter`/`AVBitStreamFilter` implementation is impossible
with distro libav* headers (opaque `AVFilterPad`, no formats constructors,
no `filter()` callback), so all processing engines are C objects governed
by `include/zff/zff_engine.h`: `alloc(opt_string)` or `create(cfg)` →
`process*` → `flush` (stateful only) → `free(&ptr)`; inputs never consumed;
errors are negative `AVERROR`; option strings are always `k=v:...`;
runtime reconfiguration (where supported) is `set_param` effective on the
next `process()`. The contract is written to survive a future in-tree
FFmpeg port unchanged.

---

## 2. Proprietary Hardware Zero-Copy: NVIDIA `NvBufSurface` & Linux DMABUF

### The Challenge
Platforms like NVIDIA Jetson (L4T / JetPack) rely on `NvBufSurface` (or `NvBuffer`), which is not part of standard FFmpeg headers (`libavutil/pixfmt.h`).

### The Solution: `AV_PIX_FMT_DRM_PRIME` Bridge
`zff` maps `NvBufSurface` to the standard Linux DRM PRIME subsystem:

```
[ NvBufSurface (Jetson NVMM) ]
       │
       │ Extract dmabuf fd (surf->surfaceList[0].bufferDesc)
       ▼
[ AVDRMFrameDescriptor ]
       │
       │ Wrapped by av_buffer_create(desc, free_nvbuf_cb, surf)
       ▼
[ AVFrame (format = AV_PIX_FMT_DRM_PRIME) ]
       │
       ├── Downstream zstr_* plugin: NvBufSurfaceFromFd(desc->objects[0].fd, &surf)
       └── Downstream standard FFmpeg filter: Direct DRM/KMS/v4l2m2m hardware access
```

### Zero-Copy Code Implementation:
```c
static void free_nvbuf_cb(void *opaque, uint8_t *data) {
    NvBufSurface *surf = (NvBufSurface *)opaque;
    NvBufSurfaceDestroy(surf);
    av_free(data); // Free the AVDRMFrameDescriptor
}

AVFrame* zff_nvbuf_wrap_frame(NvBufSurface *surf, int width, int height) {
    AVFrame *frame = av_frame_alloc();
    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_DRM_PRIME;

    AVDRMFrameDescriptor *desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
    desc->nb_objects = 1;
    desc->objects[0].fd = surf->surfaceList[0].bufferDesc;
    desc->objects[0].size = surf->surfaceList[0].dataSize;
    desc->nb_layers = 1;
    desc->layers[0].nb_planes = surf->surfaceList[0].planeParams.num_planes;

    for (int i = 0; i < desc->layers[0].nb_planes; i++) {
        desc->layers[0].planes[i].object_index = 0;
        desc->layers[0].planes[i].offset = surf->surfaceList[0].planeParams.offset[i];
        desc->layers[0].planes[i].pitch  = surf->surfaceList[0].planeParams.pitch[i];
    }

    frame->buf[0] = av_buffer_create((uint8_t*)desc, sizeof(*desc),
                                     free_nvbuf_cb, surf, 0);
    frame->data[0] = (uint8_t*)desc;
    return frame;
}

NvBufSurface* zff_nvbuf_unwrap_frame(const AVFrame *frame) {
    if (frame->format != AV_PIX_FMT_DRM_PRIME) return NULL;
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
    int dmabuf_fd = desc->objects[0].fd;
    NvBufSurface *surf = NULL;
    if (NvBufSurfaceFromFd(dmabuf_fd, (void**)&surf) == 0) {
        return surf;
    }
    return NULL;
}
```

---

## 3. Metadata & SideData Extension Mechanism

### 3.1 FourCC Custom SideData Tags
FFmpeg provides `AV_FRAME_DATA_USER_DATA` for `AVFrame`, but `AVPacket` lacks a generic user-data enum.
Because `av_packet_get_side_data()` matches integers directly without bounds checking, `zff` defines unique 32-bit FourCC tags:

```c
#define ZSTR_TAG_NVBUF ((enum AVFrameSideDataType)MKTAG('N', 'V', 'B', 'F'))
#define ZSTR_TAG_PTP   ((enum AVFrameSideDataType)MKTAG('P', 'T', 'P', 'T'))
#define ZSTR_TAG_BBOX  ((enum AVFrameSideDataType)MKTAG('B', 'B', 'O', 'X'))
#define ZSTR_TAG_USER  ((enum AVFrameSideDataType)MKTAG('Z', 'U', 'S', 'R'))
```

### 3.2 Standard Specification: FourCC Tag Branching (分流 Tag 法)

`zff` strictly adopts the **FourCC Tag Branching** approach as its standard user-data embedding mechanism across both `AVFrame` and `AVPacket`.

Each distinct domain/business payload is assigned an explicit, globally unique FourCC identifier. This ensures:
1. **Zero Demuxing Overhead**: Each downstream plugin queries precisely its required metadata using $O(1)$ lookups without parsing TLV containers.
2. **Native FFmpeg Compatibility**: Fully leverages `av_frame_get_side_data()` and `av_packet_get_side_data()`.
3. **Decoupled Lifecycles**: Modules can add, inspect, or drop individual side data entries independently.

#### Usage Example:
```c
// 1. Ingest / Producer: Embedding multiple user-data via distinct FourCC tags
AVFrameSideData *sd_ptp  = av_frame_new_side_data(frame, ZSTR_TAG_PTP, sizeof(zff_ptp_time_t));
AVFrameSideData *sd_bbox = av_frame_new_side_data(frame, ZSTR_TAG_BBOX, sizeof(zff_bbox_t));

// 2. Downstream / Consumer: Direct O(1) retrieval without interference
AVFrameSideData *found = av_frame_get_side_data(frame, ZSTR_TAG_PTP);
if (found) {
    zff_ptp_time_t *ptp = (zff_ptp_time_t *)found->data;
    // Process PTP timestamp directly...
}
```

---

## 4. Pipeline Scheduling & Data Flow

`zff` eliminates custom schedulers. Pipeline execution is driven by two standard paradigms:

### Paradigm 1: Filter Processing via `libavfilter` (Push/Pull Graph)
Filters implement `filter_frame()` and `query_formats()`. Upstream pushes `AVFrame` down the graph. FFmpeg automatically negotiates formats, inserting converters (`scale` or `resample`) when required.

### Paradigm 2: Real-time Streaming via Threaded Queues
For network protocols (e.g. ST 2110, WebRTC, RTSP Server), `zff` uses standard thread-safe FIFOs (such as `AVThreadMessageQueue` or ring buffers) to decouple network I/O from encoding and filter graphs.
