/*=============================================================================
    test_core.c — Unit Tests for libzff-core
=============================================================================*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include "zff/zff_core.h"
#include "zff/zff_hw.h"
#include "zff/zff_time.h"

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

    av_frame_free(&frame);
    // Note: memfd was closed automatically by av_buffer_create free callback
    printf("[PASS] DMABUF AVFrame wrapper passed.\n");
}

int main(void) {
    printf("========================================\n");
    printf("   Running zff-core Unit Tests\n");
    printf("========================================\n");

    assert(zff_plugins_register_all() == 0);
    test_ptp_frame();
    test_ptp_packet();
    test_ptp_to_pts();
    test_dmabuf_wrapper();

    printf("========================================\n");
    printf("   All Tests Passed Successfully!\n");
    printf("========================================\n");
    return 0;
}
