/*=============================================================================
    zff_hw.c — Hardware Zero-Copy Bridges (DMABUF & NvBufSurface)
=============================================================================*/
#include "zff/zff_hw.h"
#include <stdlib.h>
#include <unistd.h>
#include <libavutil/mem.h>
#include <libavutil/hwcontext_drm.h>

#if defined(ENABLE_JETSON) && ENABLE_JETSON
#include <nvbufsurface.h>

static void free_nvbuf_cb(void *opaque, uint8_t *data) {
    NvBufSurface *surf = (NvBufSurface *)opaque;
    if (surf) {
        NvBufSurfaceDestroy(surf);
    }
    av_free(data); // Free the AVDRMFrameDescriptor
}

AVFrame* zff_nvbuf_wrap_frame(struct NvBufSurface *surf, int width, int height) {
    if (!surf) return NULL;
    AVFrame *frame = av_frame_alloc();
    if (!frame) return NULL;

    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_DRM_PRIME;

    AVDRMFrameDescriptor *desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc) {
        av_frame_free(&frame);
        return NULL;
    }

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
    if (!frame->buf[0]) {
        av_free(desc);
        av_frame_free(&frame);
        return NULL;
    }
    frame->data[0] = (uint8_t*)desc;
    return frame;
}

struct NvBufSurface* zff_nvbuf_unwrap_frame(const AVFrame *frame) {
    if (!frame || frame->format != AV_PIX_FMT_DRM_PRIME || !frame->data[0]) return NULL;
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
    if (desc->nb_objects < 1) return NULL;
    int dmabuf_fd = desc->objects[0].fd;

    NvBufSurface *surf = NULL;
    if (NvBufSurfaceFromFd(dmabuf_fd, (void**)&surf) == 0) {
        return surf;
    }
    return NULL;
}
#else
AVFrame* zff_nvbuf_wrap_frame(struct NvBufSurface *surf, int width, int height) {
    (void)surf; (void)width; (void)height;
    return NULL;
}

struct NvBufSurface* zff_nvbuf_unwrap_frame(const AVFrame *frame) {
    (void)frame;
    return NULL;
}
#endif

static void free_dmabuf_cb(void *opaque, uint8_t *data) {
    int fd = (int)(intptr_t)opaque;
    if (fd >= 0) {
        close(fd);
    }
    av_free(data);
}

AVFrame* zff_dmabuf_wrap_frame(int dmabuf_fd, size_t size, int width, int height, uint32_t drm_format) {
    if (dmabuf_fd < 0) return NULL;
    AVFrame *frame = av_frame_alloc();
    if (!frame) return NULL;

    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_DRM_PRIME;

    AVDRMFrameDescriptor *desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc) {
        av_frame_free(&frame);
        return NULL;
    }

    desc->nb_objects = 1;
    desc->objects[0].fd = dmabuf_fd;
    desc->objects[0].size = size;
    desc->nb_layers = 1;
    desc->layers[0].format = drm_format;
    desc->layers[0].nb_planes = 1;
    desc->layers[0].planes[0].object_index = 0;
    desc->layers[0].planes[0].offset = 0;
    desc->layers[0].planes[0].pitch = width * 4; // default assumption, caller can customize

    frame->buf[0] = av_buffer_create((uint8_t*)desc, sizeof(*desc),
                                     free_dmabuf_cb, (void*)(intptr_t)dmabuf_fd, 0);
    if (!frame->buf[0]) {
        av_free(desc);
        av_frame_free(&frame);
        return NULL;
    }
    frame->data[0] = (uint8_t*)desc;
    return frame;
}
