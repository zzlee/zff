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
 */
AVFrame* zff_dmabuf_wrap_frame(int dmabuf_fd, size_t size, int width, int height, uint32_t drm_format);

#ifdef __cplusplus
}
#endif
