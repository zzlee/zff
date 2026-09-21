/*=============================================================================
    test_webrtc_device.c — zstr_webrtc muxer/demuxer file-signaled loopback

    Demuxer thread (subscriber) + main-thread muxer (publisher) exchange
    SDP via temp files, then video (H264) + audio (Opus) packets round-trip
    through real PeerConnections. Payloads are self-describing so stream
    order does not matter.
 =============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>

#include "zff/plugins/zstr_webrtc.h"
#include "zff/zff_core.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

static char g_offer[256];
static char g_answer[256];

/* Video NAL: Annex-B IDR, first content byte 0x65; audio: 0xAA fill */
#define N_VIDEO 5
#define N_AUDIO 5

typedef struct {
    int got_video;
    int got_audio;
    int errors;
} demux_result_t;

static void *demux_thread(void *arg)
{
    demux_result_t *res = arg;

    const AVInputFormat *ifmt = zff_find_input_format("zstr_webrtc");
    if (!ifmt) {
        res->errors++;
        return NULL;
    }

    AVFormatContext *ic = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "offer_file", g_offer, 0);
    av_dict_set(&opts, "answer_file", g_answer, 0);
    av_dict_set(&opts, "handshake_timeout", "20000", 0);
    if (avformat_open_input(&ic, "webrtc://subscriber", ifmt, &opts) < 0) {
        res->errors++;
        av_dict_free(&opts);
        return NULL;
    }
    av_dict_free(&opts);

    if (!ic || ic->nb_streams != 2) {
        res->errors++;
        if (ic) avformat_close_input(&ic);
        return NULL;
    }

    AVPacket *pkt = av_packet_alloc();
    int total = N_VIDEO + N_AUDIO;
    for (int i = 0; i < total + 4; i++) {
        int ret = av_read_frame(ic, pkt);
        if (ret < 0) break;
        if (pkt->size >= 5 && pkt->data[0] == 0 && pkt->data[1] == 0 &&
            pkt->data[2] == 0 && pkt->data[3] == 1 && pkt->data[4] == 0x65) {
            res->got_video++;
        } else if (pkt->size == 32 && pkt->data[0] == 0xAA) {
            res->got_audio++;
        }
        av_packet_unref(pkt);
        if (res->got_video >= N_VIDEO && res->got_audio >= N_AUDIO) break;
    }
    av_packet_free(&pkt);
    avformat_close_input(&ic);
    return NULL;
}

static void test_webrtc_mux_demux(void)
{
    printf("[TEST] WebRTC muxer/demuxer file-signaled loopback...\n");

    snprintf(g_offer, sizeof(g_offer), "/tmp/zstr_webrtc_test_%d_offer.sdp", (int)getpid());
    snprintf(g_answer, sizeof(g_answer), "/tmp/zstr_webrtc_test_%d_answer.sdp", (int)getpid());
    unlink(g_offer);
    unlink(g_answer);

    demux_result_t res;
    memset(&res, 0, sizeof(res));
    pthread_t th;
    CHECK(pthread_create(&th, NULL, demux_thread, &res) == 0);

    /* Publisher (main thread) */
    const AVOutputFormat *ofmt = zff_find_output_format("zstr_webrtc");
    CHECK(ofmt != NULL);

    AVFormatContext *oc = NULL;
    CHECK(avformat_alloc_output_context2(&oc, ofmt, "zstr_webrtc", "webrtc://publisher") >= 0);

    AVStream *vst = avformat_new_stream(oc, NULL);
    CHECK(vst != NULL);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id = AV_CODEC_ID_H264;
    vst->codecpar->width = 320;
    vst->codecpar->height = 240;
    vst->time_base = (AVRational){ 1, 90000 };

    AVStream *ast = avformat_new_stream(oc, NULL);
    CHECK(ast != NULL);
    ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    ast->codecpar->codec_id = AV_CODEC_ID_OPUS;
    ast->codecpar->sample_rate = 48000;
    ast->codecpar->ch_layout.nb_channels = 1;
    av_channel_layout_default(&ast->codecpar->ch_layout, 1);
    ast->time_base = (AVRational){ 1, 48000 };

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "offer_file", g_offer, 0);
    av_dict_set(&opts, "answer_file", g_answer, 0);
    av_dict_set(&opts, "handshake_timeout", "20000", 0);
    CHECK(avformat_write_header(oc, &opts) >= 0);
    av_dict_free(&opts);

    for (int i = 0; i < N_VIDEO; i++) {
        uint8_t nal[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, (uint8_t)i, 0x21 };
        AVPacket *pkt = av_packet_alloc();
        av_new_packet(pkt, sizeof(nal));
        memcpy(pkt->data, nal, sizeof(nal));
        pkt->stream_index = 0;
        pkt->pts = i * 3000;
        pkt->dts = pkt->pts;
        pkt->time_base = vst->time_base;
        CHECK(av_write_frame(oc, pkt) == 0);
        av_packet_free(&pkt);
    }
    for (int i = 0; i < N_AUDIO; i++) {
        AVPacket *pkt = av_packet_alloc();
        av_new_packet(pkt, 32);
        memset(pkt->data, 0xAA, 32);
        pkt->data[31] = (uint8_t)i;
        pkt->stream_index = 1;
        pkt->pts = i * 960;
        pkt->dts = pkt->pts;
        pkt->time_base = ast->time_base;
        CHECK(av_write_frame(oc, pkt) == 0);
        av_packet_free(&pkt);
    }

    pthread_join(th, NULL);
    CHECK(res.errors == 0);
    CHECK(res.got_video == N_VIDEO);
    CHECK(res.got_audio == N_AUDIO);

    av_write_trailer(oc);
    avformat_free_context(oc);
    unlink(g_offer);
    unlink(g_answer);

    printf("[PASS] Muxer/demuxer loopback passed (%d video + %d audio).\n",
           res.got_video, res.got_audio);
}

int main(void)
{
    printf("====================================================\n");
    printf("      Running zstr_webrtc Device Tests               \n");
    printf("====================================================\n");

    zff_plugins_register_all();
    CHECK(zff_find_input_format("zstr_webrtc") != NULL);
    CHECK(zff_find_output_format("zstr_webrtc") != NULL);
    test_webrtc_mux_demux();

    printf("====================================================\n");
    printf("   All zstr_webrtc Device Tests Passed!             \n");
    printf("====================================================\n");
    return 0;
}
