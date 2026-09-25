/*=============================================================================
    zff_hw.h — Hardware Zero-Copy Bridges (NvBufSurface, DMABUF, CUDA)
=============================================================================*/
#pragma once

#include "zff_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of NVIDIA Jetson surface */
struct NvBufSurface;

/* ── DRM fourcc constants (avoid pulling libdrm headers into the public API) ── */
#define ZFF_DRM_FORMAT_NV12 0x3231564eu /* "NV12" */
#define ZFF_DRM_FORMAT_NV16 0x3631564eu /* "NV16" */
#define ZFF_DRM_FORMAT_NV21 0x3132564eu /* "NV21" */

/**
 * Per-plane layout for AVDRMFrameDescriptor layers when wrapping a dmabuf.
 * object_index must be 0 (single-object buffers; zff core SDK owns one fd).
 */
typedef struct ZffDRMPlane {
    int      object_index;
    uint32_t offset; /**< byte offset of the plane inside the object */
    uint32_t pitch;  /**< bytes per row */
} ZffDRMPlane;

/**
 * DMABUF buffer info carried as FourCC side data (ZSTR_TAG_DMAB) on an
 * AVPacket produced by zero-copy devices (e.g. zstr_v4l2 memory_type=dmabuf).
 * Consumers unwrap the fd with zff_dmabuf_wrap_frame_planes() to get an
 * AV_PIX_FMT_DRM_PRIME frame without any pixel copy.
 */
typedef struct ZffDMABufInfo {
    int      fd;          /**< dma-buf file descriptor (owned by the packet) */
    uint32_t size;        /**< dma-buf object size in bytes */
    uint32_t bytesused;   /**< bytes actually filled by the producer */
    int      width;
    int      height;
    uint32_t drm_format;  /**< ZFF_DRM_FORMAT_* (0 = generic/unknown) */
    uint32_t y_offset;    /**< luma plane offset (0 for packed formats) */
    uint32_t y_pitch;     /**< luma plane bytes-per-row */
    uint32_t uv_offset;   /**< chroma plane offset (0 if not 2-plane) */
    uint32_t uv_pitch;    /**< chroma plane bytes-per-row (0 if not 2-plane) */
} ZffDMABufInfo;

/**
 * Wrap a Jetson NvBufSurface into an AVFrame (format = AV_PIX_FMT_DRM_PRIME).
 * Manages the surface lifecycle using av_buffer_create.
 *
 * @param surf   Valid pointer to allocated NvBufSurface.
 * @param width  Frame width in pixels.
 * @param height Frame height in pixels.
 * @return Allocated AVFrame or NULL on failure.
 */
AVFrame* zff_nvbuf_wrap_frame(struct NvBufSurface *surf, int width, int height);

/**
 * Extract the underlying NvBufSurface pointer from an AVFrame.
 *
 * @param frame  AVFrame with format == AV_PIX_FMT_DRM_PRIME.
 * @return Pointer to NvBufSurface, or NULL if not a Jetson buffer.
 */
struct NvBufSurface* zff_nvbuf_unwrap_frame(const AVFrame *frame);

/**
 * Wrap an arbitrary Linux DMABUF file descriptor into an AVFrame.
 *
 * Plane layout is derived from drm_format when it is a known semi-planar
 * format (ZFF_DRM_FORMAT_NV12/NV16: two planes, dense pitch == width),
 * otherwise a single plane with pitch == width*4 is assumed. Use
 * zff_dmabuf_wrap_frame_planes() for explicit control (padded pitches,
 * non-zero offsets, other formats).
 *
 * The AVFrame's buffer ref owns the fd: it is closed when the frame is freed.
 */
AVFrame* zff_dmabuf_wrap_frame(int dmabuf_fd, size_t size, int width, int height, uint32_t drm_format);

/**
 * Wrap a Linux DMABUF fd into an AVFrame with an explicit plane layout.
 *
 * @param planes     array of ZffDRMPlane (object_index must be 0), or NULL to
 *                   fall back to a single plane with pitch == width*4.
 * @param nb_planes  number of planes (1..4). Ignored if planes == NULL.
 * Same fd ownership semantics as zff_dmabuf_wrap_frame().
 */
AVFrame* zff_dmabuf_wrap_frame_planes(int dmabuf_fd, size_t size, int width, int height,
                                      uint32_t drm_format,
                                      const ZffDRMPlane *planes, int nb_planes);

/**
 * Attach DMABUF buffer info to an AVPacket using FourCC side data (ZSTR_TAG_DMAB).
 * Used by zero-copy devices so the fd can ride along with the packet.
 */
int zff_packet_set_dmabuf(AVPacket *pkt, const ZffDMABufInfo *info);

/**
 * Retrieve DMABUF buffer info from an AVPacket.
 * Returns 0 and fills *out on success, negative AVERROR if absent.
 */
int zff_packet_get_dmabuf(const AVPacket *pkt, ZffDMABufInfo *out);

#ifdef __cplusplus
}
#endif
