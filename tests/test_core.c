/*=============================================================================
    test_core.c — Unit Tests for libzff-core
=============================================================================*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include "zff/zff_core.h"
#include "zff/zff_hw.h"
#include "zff/zff_time.h"
#include <libavutil/hwcontext_drm.h>

static void test_ptp_frame(void) {
    printf("[TEST] Testing PTP FourCC side data on AVFrame...\n");
    AVFrame *frame = av_frame_alloc();
    assert(frame != NULL);

    zff_ptp_time_t ptp_in = {
        .tai_nanoseconds = 1718000000123456789ULL,
        .domain = 1
    };

    assert(zff_frame_set_ptp(frame, &ptp_in) == 0);

    zff_ptp_time_t ptp_out = {0};
    assert(zff_frame_get_ptp(frame, &ptp_out) == 0);
    assert(ptp_out.tai_nanoseconds == ptp_in.tai_nanoseconds);
    assert(ptp_out.domain == ptp_in.domain);

    av_frame_free(&frame);
    assert(frame == NULL);
    printf("[PASS] PTP FourCC side data on AVFrame passed.\n");
}

static void test_ptp_packet(void) {
    printf("[TEST] Testing PTP FourCC side data on AVPacket...\n");
    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);

    zff_ptp_time_t ptp_in = {
        .tai_nanoseconds = 987654321000000ULL,
        .domain = 2
    };

    assert(zff_packet_set_ptp(pkt, &ptp_in) == 0);

    zff_ptp_time_t ptp_out = {0};
    assert(zff_packet_get_ptp(pkt, &ptp_out) == 0);
    assert(ptp_out.tai_nanoseconds == ptp_in.tai_nanoseconds);
    assert(ptp_out.domain == ptp_in.domain);

    av_packet_free(&pkt);
    assert(pkt == NULL);
    printf("[PASS] PTP FourCC side data on AVPacket passed.\n");
}

static void test_ptp_to_pts(void) {
    printf("[TEST] Testing PTP nanoseconds to PTS conversion...\n");
    AVRational time_base_90k = { 1, 90000 };
    uint64_t one_sec_nanos = 1000000000ULL;

    int64_t pts = zff_ptp_to_pts(one_sec_nanos, time_base_90k);
    assert(pts == 90000);

    AVRational time_base_ms = { 1, 1000 };
    pts = zff_ptp_to_pts(one_sec_nanos, time_base_ms);
    assert(pts == 1000);

    printf("[PASS] PTP nanoseconds to PTS conversion passed.\n");
}

static void test_dmabuf_wrapper(void) {
    printf("[TEST] Testing DMABUF AVFrame wrapper...\n");
    int memfd = memfd_create("test_dmabuf", 0);
    assert(memfd >= 0);

    size_t sz = 1920 * 1080 * 4;
    assert(ftruncate(memfd, sz) == 0);

    AVFrame *frame = zff_dmabuf_wrap_frame(memfd, sz, 1920, 1080, 0);
    assert(frame != NULL);
    assert(frame->format == AV_PIX_FMT_DRM_PRIME);
    assert(frame->width == 1920);
    assert(frame->height == 1080);

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*)frame->data[0];
    assert(desc != NULL);
    assert(desc->nb_objects == 1);
    assert(desc->objects[0].fd == memfd);
    assert(desc->objects[0].size == sz);
    assert(desc->nb_layers == 1);
    assert(desc->layers[0].nb_planes == 1);
    assert(desc->layers[0].planes[0].pitch == 1920 * 4);

    av_frame_free(&frame);
    // Note: memfd was closed automatically by av_buffer_create free callback
    printf("[PASS] DMABUF AVFrame wrapper passed.\n");

    /* ── NV12: 2 planes with correct pitch/offset (the zero-copy glue path) ── */
    printf("[TEST] Testing NV12 DMABUF wrap (2 planes)...\n");
    int nvfd = memfd_create("test_nv12", 0);
    assert(nvfd >= 0);
    size_t nv12_sz = 640 * 480 * 3 / 2;
    assert(ftruncate(nvfd, nv12_sz) == 0);

    AVFrame *nv12 = zff_dmabuf_wrap_frame(nvfd, nv12_sz, 640, 480, ZFF_DRM_FORMAT_NV12);
    assert(nv12 != NULL);
    AVDRMFrameDescriptor *d = (AVDRMFrameDescriptor*)nv12->data[0];
    assert(d != NULL);
    assert(d->layers[0].format == ZFF_DRM_FORMAT_NV12);
    assert(d->layers[0].nb_planes == 2);
    assert(d->layers[0].planes[0].object_index == 0);
    assert(d->layers[0].planes[0].offset == 0);
    assert(d->layers[0].planes[0].pitch == 640);
    assert(d->layers[0].planes[1].offset == 640 * 480);
    assert(d->layers[0].planes[1].pitch == 640);
    assert(d->objects[0].fd == nvfd);
    av_frame_free(&nv12);
    printf("[PASS] NV12 DMABUF wrap passed.\n");

    /* ── Explicit plane layout (padded pitch, custom offsets) ─────────────── */
    printf("[TEST] Testing DMABUF wrap with explicit planes...\n");
    int pfd = memfd_create("test_planes", 0);
    assert(pfd >= 0);
    size_t pad_sz = 1280 * 480 + 640 * 240;
    assert(ftruncate(pfd, pad_sz) == 0);

    const ZffDRMPlane planes[2] = {
        { .object_index = 0, .offset = 0,        .pitch = 1280 }, /* Y padded to 1280 */
        { .object_index = 0, .offset = 1280*480, .pitch = 640  }, /* UV dense 640    */
    };
    AVFrame *pad = zff_dmabuf_wrap_frame_planes(pfd, pad_sz, 640, 480,
                                                ZFF_DRM_FORMAT_NV12, planes, 2);
    assert(pad != NULL);
    AVDRMFrameDescriptor *pd = (AVDRMFrameDescriptor*)pad->data[0];
    assert(pd->layers[0].nb_planes == 2);
    assert(pd->layers[0].planes[0].pitch == 1280);
    assert(pd->layers[0].planes[0].offset == 0);
    assert(pd->layers[0].planes[1].offset == 1280 * 480);
    assert(pd->layers[0].planes[1].pitch == 640);
    av_frame_free(&pad);
    printf("[PASS] Explicit planes DMABUF wrap passed.\n");

    /* ── DMABUF info side data on AVPacket (ZSTR_TAG_DMAB) ────────────────── */
    printf("[TEST] Testing DMABUF info packet side data round-trip...\n");
    int sfd = memfd_create("test_side", 0);
    assert(sfd >= 0);
    assert(ftruncate(sfd, nv12_sz) == 0);
    ZffDMABufInfo info = {0};
    info.fd         = sfd;
    info.size       = (uint32_t)nv12_sz;
    info.bytesused  = (uint32_t)nv12_sz;
    info.width      = 640;
    info.height     = 480;
    info.drm_format = ZFF_DRM_FORMAT_NV12;
    info.y_offset   = 0;
    info.y_pitch    = 640;
    info.uv_offset  = 640 * 480;
    info.uv_pitch   = 640;

    AVPacket *pkt = av_packet_alloc();
    assert(pkt != NULL);
    assert(zff_packet_set_dmabuf(pkt, &info) == 0);
    ZffDMABufInfo out = {0};
    assert(zff_packet_get_dmabuf(pkt, &out) == 0);
    assert(memcmp(&info, &out, sizeof(info)) == 0);
    av_packet_free(&pkt);
    close(sfd);
    printf("[PASS] DMABUF info side data round-trip passed.\n");
}

int main(void) {
    printf("========================================\n");
    printf("   Running zff-core Unit Tests\n");
    printf("========================================\n");

    test_ptp_frame();
    test_ptp_packet();
    test_ptp_to_pts();
    test_dmabuf_wrapper();

    printf("========================================\n");
    printf("   All Tests Passed Successfully!\n");
    printf("========================================\n");
    return 0;
}
