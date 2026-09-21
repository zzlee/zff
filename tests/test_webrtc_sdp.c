/*=============================================================================
    test_webrtc_sdp.c — Unit tests for zstr SDP compat surgery

    Covers filter (strip list + TWCC keep-deviation), codec selection
    (default + override preference), and local compat normalization.
 =============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "zff/plugins/zstr_webrtc_sdp.h"

/* CHECK: always evaluated (assert() is compiled out under NDEBUG/Release) */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        fflush(stderr); \
        abort(); \
    } \
} while (0)

static void test_filter(void)
{
    printf("[TEST] SDP filter strip list...\n");
    const char *in =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 126\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=extmap:1 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
        "a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        "a=extmap:4 urn:3gpp:video-orientation\r\n"
        "a=rtpmap:126 H264/90000\r\n"
        "a=rtcp-fb:126 transport-cc\r\n"
        "a=rtcp-fb:126 ccm fir\r\n"
        "a=rtcp-fb:126 nack\r\n";
    char *out = zstr_sdp_filter(in);
    CHECK(out != NULL);
    /* Stripped */
    CHECK(strstr(out, "abs-send-time") == NULL);
    CHECK(strstr(out, "video-orientation") == NULL);
    CHECK(strstr(out, "ccm fir") == NULL);
    /* Kept: TWCC extmap (zff deviation) + transport-cc fb + mid extmap */
    CHECK(strstr(out, "transport-wide-cc-extensions-01") != NULL);
    CHECK(strstr(out, "a=rtcp-fb:126 transport-cc") != NULL);
    CHECK(strstr(out, "sdes:mid") != NULL);
    CHECK(strstr(out, "a=rtpmap:126 H264/90000") != NULL);
    free(out);
    printf("[PASS] SDP filter passed.\n");
}

static const char *kOffer2 =
    "v=0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 126\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "a=rtcp-fb:96 nack\r\n"
    "a=rtpmap:126 H264/90000\r\n"
    "a=fmtp:126 packetization-mode=1\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:0 PCMU/8000\r\n";

static void test_select_default(void)
{
    printf("[TEST] Codec selection with default preference (H264 > VP8)...\n");
    char video[32] = "", audio[32] = "";
    char *out = zstr_sdp_select_codecs(kOffer2, NULL,
                                       video, sizeof(video), audio, sizeof(audio));
    CHECK(out != NULL);
    CHECK(strcmp(video, "H264") == 0);
    CHECK(strcmp(audio, "opus") == 0);
    /* m-line narrowed to the winner */
    CHECK(strstr(out, "m=video 9 UDP/TLS/RTP/SAVPF 126\r\n") != NULL);
    /* Loser lines gone, winner fmtp kept */
    CHECK(strstr(out, "VP8") == NULL);
    CHECK(strstr(out, "a=fmtp:126 packetization-mode=1") != NULL);
    CHECK(strstr(out, "a=rtpmap:0 PCMU") == NULL);
    free(out);
    printf("[PASS] Default selection passed.\n");
}

static void test_select_override(void)
{
    printf("[TEST] Codec selection with override preference (VP8,H264)...\n");
    char video[32] = "", audio[32] = "";
    char *out = zstr_sdp_select_codecs(kOffer2, "VP8,H264",
                                       video, sizeof(video), audio, sizeof(audio));
    CHECK(out != NULL);
    CHECK(strcmp(video, "VP8") == 0);
    CHECK(strstr(out, "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n") != NULL);
    CHECK(strstr(out, "H264") == NULL);
    free(out);
    printf("[PASS] Override selection passed.\n");
}

static void test_compat_local(void)
{
    printf("[TEST] Local SDP compat normalization...\n");
    const char *in =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 126\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:video0\r\n"
        "a=rtpmap:126 H264/90000\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:audio0\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";
    char *out = zstr_sdp_compat_local(in);
    CHECK(out != NULL);
    CHECK(strstr(out, "a=group:BUNDLE video0 audio0") != NULL);
    CHECK(strstr(out, "a=rtcp-mux") != NULL);
    CHECK(strstr(out, "a=msid:zff-stream") != NULL);
    CHECK(strstr(out, "cname:zff-cname") != NULL);
    /* Original content preserved */
    CHECK(strstr(out, "a=rtpmap:126 H264/90000") != NULL);
    CHECK(strstr(out, "a=mid:video0") != NULL);
    free(out);
    printf("[PASS] Compat normalization passed.\n");
}

int main(void)
{
    printf("====================================================\n");
    printf("       Running zstr_webrtc SDP Compat Tests          \n");
    printf("====================================================\n");

    test_filter();
    test_select_default();
    test_select_override();
    test_compat_local();

    printf("====================================================\n");
    printf("    All zstr_webrtc SDP Tests Passed!               \n");
    printf("====================================================\n");
    return 0;
}
