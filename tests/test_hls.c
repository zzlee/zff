/*=============================================================================
    test_hls.c — zstr_hls_sink segmenter + demux-back verification
 =============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libavformat/avformat.h>

#include "zff/plugins/zstr_hls_sink.h"
#include "zff/zff_core.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

/* Minimal avcC: 1 SPS (13B) + 1 PPS (4B), content opaque for the muxer */
static const uint8_t FAKE_AVCC[] = {
    0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x0d,
    0x67, 0x42, 0x00, 0x1f, 0x96, 0x54, 0x05, 0x01,
    0xed, 0x80, 0x80, 0x80, 0xa0, 0x01, 0x00, 0x04,
    0x68, 0xce, 0x3c, 0x80
};

static int count_ext(const char *dir, const char *ext)
{
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcmp(dot, ext) == 0) n++;
    }
    closedir(d);
    return n;
}

static void test_hls_mux_and_demux_back(void)
{
    printf("[TEST] HLS mux to dir + demux-back via playlist...\n");

    char dir[] = "/tmp/zstr_hls_test_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char playlist[512];
    snprintf(playlist, sizeof(playlist), "%s/out.m3u8", dir);

    const AVOutputFormat *out_fmt = zff_find_output_format("zstr_hls_sink");
    CHECK(out_fmt != NULL);

    AVFormatContext *oc = NULL;
    CHECK(avformat_alloc_output_context2(&oc, out_fmt, "zstr_hls_sink", playlist) >= 0);

    AVStream *vst = avformat_new_stream(oc, NULL);
    CHECK(vst != NULL);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id = AV_CODEC_ID_H264;
    vst->codecpar->width = 320;
    vst->codecpar->height = 240;
    vst->codecpar->extradata = av_malloc(sizeof(FAKE_AVCC) + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(vst->codecpar->extradata, FAKE_AVCC, sizeof(FAKE_AVCC));
    vst->codecpar->extradata_size = sizeof(FAKE_AVCC);
    vst->time_base = (AVRational){ 1, 90000 };

    AVStream *ast = avformat_new_stream(oc, NULL);
    CHECK(ast != NULL);
    ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    ast->codecpar->codec_id = AV_CODEC_ID_AAC;
    ast->codecpar->sample_rate = 44100;
    av_channel_layout_default(&ast->codecpar->ch_layout, 2);
    ast->codecpar->extradata = av_malloc(2 + AV_INPUT_BUFFER_PADDING_SIZE);
    ast->codecpar->extradata[0] = 0x12;
    ast->codecpar->extradata[1] = 0x10;
    ast->codecpar->extradata_size = 2;
    ast->time_base = (AVRational){ 1, 44100 };

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "hls_time", "1", 0);
    av_dict_set(&opts, "hls_list_size", "0", 0); /* keep all segments */
    CHECK(avformat_write_header(oc, &opts) >= 0);
    av_dict_free(&opts);

    /* 60 video frames (2s @30fps) + matching audio: forces >= 1 segment */
    static const uint8_t idr[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x11, 0x22 };
    static const uint8_t nonidr[] = { 0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x33 };
    static uint8_t aac[64];
    memset(aac, 0x21, sizeof(aac));
    for (int i = 0; i < 60; i++) {
        const uint8_t *nal = (i % 30 == 0) ? idr : nonidr;
        size_t nalsz = (i % 30 == 0) ? sizeof(idr) : sizeof(nonidr);
        AVPacket *vpkt = av_packet_alloc();
        av_new_packet(vpkt, nalsz);
        memcpy(vpkt->data, nal, nalsz);
        vpkt->stream_index = 0;
        vpkt->pts = i * 3000;
        vpkt->dts = vpkt->pts;
        vpkt->duration = 3000;
        if (i % 30 == 0) vpkt->flags |= AV_PKT_FLAG_KEY;
        CHECK(av_write_frame(oc, vpkt) == 0);
        av_packet_free(&vpkt);

        /* ~2 AAC frames per video frame (1024 @44.1kHz) */
        for (int j = 0; j < 2; j++) {
            AVPacket *apkt = av_packet_alloc();
            av_new_packet(apkt, sizeof(aac));
            memcpy(apkt->data, aac, sizeof(aac));
            apkt->stream_index = 1;
            int64_t apts = ((int64_t)i * 2 + j) * 1024;
            apkt->pts = apts;
            apkt->dts = apts;
            apkt->duration = 1024;
            CHECK(av_write_frame(oc, apkt) == 0);
            av_packet_free(&apkt);
        }
    }
    CHECK(av_write_trailer(oc) == 0);
    avformat_free_context(oc);

    /* Playlist + segments exist */
    CHECK(count_ext(dir, ".m3u8") >= 1);
    int nseg = count_ext(dir, ".ts");
    CHECK(nseg >= 1);
    printf("[INFO] Playlist + %d MPEG-TS segment(s) written.\n", nseg);

    /* Demux back through the playlist with the NATIVE hls demuxer */
    AVFormatContext *ic = NULL;
    CHECK(avformat_open_input(&ic, playlist, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(ic, NULL) >= 0);
    int got_video = 0, got_audio = 0;
    AVPacket *rx = av_packet_alloc();
    for (int i = 0; i < 400; i++) {
        if (av_read_frame(ic, rx) < 0) break;
        if (rx->stream_index < (int)ic->nb_streams) {
            enum AVMediaType t = ic->streams[rx->stream_index]->codecpar->codec_type;
            if (t == AVMEDIA_TYPE_VIDEO) got_video++;
            else if (t == AVMEDIA_TYPE_AUDIO) got_audio++;
        }
        av_packet_unref(rx);
    }
    av_packet_free(&rx);
    avformat_close_input(&ic);
    printf("[INFO] Demuxed back %d video + %d audio packets.\n", got_video, got_audio);
    CHECK(got_video > 0);
    CHECK(got_audio > 0);

    /* Cleanup dir */
    DIR *d = opendir(dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);

    printf("[PASS] HLS mux + demux-back passed.\n");
}

int main(void)
{
    printf("====================================================\n");
    printf("           Running zstr HLS Sink Tests              \n");
    printf("====================================================\n");

    zff_plugins_register_all();
    test_hls_mux_and_demux_back();

    printf("====================================================\n");
    printf("       All zstr HLS Tests Passed!                   \n");
    printf("====================================================\n");
    return 0;
}
