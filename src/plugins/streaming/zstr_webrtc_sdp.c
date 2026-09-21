/*=============================================================================
    zstr_webrtc_sdp.c — SDP compatibility surgery for browser interop

    Ported from zstreamer `webrtc_endpoint.c` (WebRTC Phase 8: Chrome compat).
    Framework deltas:
    - ZST_LOG_* -> av_log
    - zst_webrtc_* -> zstr_sdp_* (pure string in/out, malloc'd results)
    - msid/cname branding zstreamer-* -> zff-*
    - DEVIATION (documented): zstreamer strips transport-cc extmap/rtcp-fb
      lines BEFORE TWCC parsing, which silently disables its own congestion
      control with real peers. zff keeps transport-wide-cc lines (required
      by zstr_webrtc_twcc) and strips only the rest; TWCC parsing runs on
      the raw offer first in set_remote_description().
 =============================================================================*/
#include "zff/plugins/zstr_webrtc_sdp.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <libavutil/log.h>

static const char* g_default_video_prefs[] = {
    "H264", "VP8", "VP9", "H265", "AV1", NULL
};
static const char* g_default_audio_prefs[] = {
    "opus", "PCMU", "PCMA", "AAC", NULL
};

/* Max codecs per media section we track */
#define MAX_CODECS_PER_SECTION 32

typedef struct {
    int  pt;             /* payload type number */
    char name[32];       /* codec name from a=rtpmap, e.g. "H264" */
    int  prio;           /* preference rank (0 = best) */
} sdp_codec_entry_t;

/**
 * Compute the preference rank for a codec name given a preference list.
 * Returns 0 for highest priority, higher for lower priority, INT_MAX if not found.
 */
static int
codec_pref_rank(const char* codec_name, const char** pref_list)
{
    for (int i = 0; pref_list[i]; i++) {
        if (strcasecmp(codec_name, pref_list[i]) == 0) return i;
    }
    return 9999;
}

/**
 * Build a preference list from a user-provided comma-separated string.
 * Returns a NULL-terminated array of strings. Caller must free the returned
 * array and each string.  Returns NULL if preference is NULL/empty.
 */
static const char**
parse_codec_preference(const char* pref)
{
    if (!pref || !pref[0]) return NULL;

    /* Count tokens */
    uint32_t count = 1;
    for (const char* p = pref; *p; p++) {
        if (*p == ',') count++;
    }

    const char** list = calloc(count + 1, sizeof(char*));
    if (!list) return NULL;

    uint32_t idx = 0;
    const char* start = pref;
    while (*start && idx < count) {
        start += strspn(start, " \t");
        if (!*start) break;

        const char* end = strchr(start, ',');
        size_t len = end ? (size_t)(end - start) : strlen(start);

        /* Trim trailing whitespace */
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
            len--;
        }

        if (len > 0) {
            list[idx++] = strndup(start, len);
        }

        if (!end) break;
        start = end + 1;
    }
    list[idx] = NULL;
    return list;
}

static void
free_codec_preference(const char** list)
{
    if (!list) return;
    for (int i = 0; list[i]; i++) free((char*)list[i]);
    free(list);
}

char*
zstr_sdp_select_codecs(const char* sdp, const char* preference,
                         char* selected_video_out, size_t video_out_len,
                         char* selected_audio_out, size_t audio_out_len)
{
    if (!sdp) return NULL;

    /* Build preference lists */
    const char** user_prefs = parse_codec_preference(preference);

    size_t sdp_len = strlen(sdp);
    size_t out_cap = sdp_len + 512;
    char* out = malloc(out_cap);
    if (!out) { free_codec_preference(user_prefs); return NULL; }
    out[0] = '\0';
    size_t out_len = 0;

    /* We accumulate lines for each media section, then decide what to keep */
    /* Simple two-pass approach:
     *   Pass 1: Scan the SDP to find codec info per media section
     *   Pass 2: Rewrite, keeping only the selected codec's lines
     */

    /* ── Structures to hold per-section info ─────────────────────────── */
    typedef struct {
        bool    is_audio;
        int     num_codecs;
        sdp_codec_entry_t codecs[MAX_CODECS_PER_SECTION];
        int     selected_pt;    /* best codec payload type */
        char    selected_name[32];
    } media_section_t;

    #define MAX_MEDIA_SECTIONS 8
    media_section_t sections[MAX_MEDIA_SECTIONS];
    int num_sections = 0;

    /* ── Pass 1: scan for codecs ─────────────────────────────────────── */
    const char* line = sdp;
    int cur_section = -1;

    while (*line) {
        /* Bolt optimization: Use SIMD-optimized strchr to find EOL instead of manual loop */
        const char* next_line = strchr(line, '\n');
        if (!next_line) next_line = line + strlen(line);

        if (strncmp(line, "m=", 2) == 0 && num_sections < MAX_MEDIA_SECTIONS) {
            cur_section = num_sections++;
            memset(&sections[cur_section], 0, sizeof(sections[cur_section]));
            sections[cur_section].is_audio = (strncmp(line, "m=audio", 7) == 0);
            sections[cur_section].selected_pt = -1;
        }

        /* Parse a=rtpmap:<pt> <name>/<clock> lines */
        if (cur_section >= 0 && strncmp(line, "a=rtpmap:", 9) == 0) {
            media_section_t* sec = &sections[cur_section];
            if (sec->num_codecs < MAX_CODECS_PER_SECTION) {
                int pt = 0;
                char cname[32] = {0};
                if (sscanf(line + 9, "%d %31[^/\r\n]", &pt, cname) >= 2) {
                    sdp_codec_entry_t* ce = &sec->codecs[sec->num_codecs];
                    ce->pt = pt;
                    snprintf(ce->name, sizeof(ce->name), "%s", cname);

                    /* Compute priority */
                    const char** vprefs = user_prefs ? user_prefs : g_default_video_prefs;
                    const char** aprefs = user_prefs ? user_prefs : g_default_audio_prefs;
                    ce->prio = codec_pref_rank(cname, sec->is_audio ? aprefs : vprefs);

                    sec->num_codecs++;
                }
            }
        }

        line = next_line;
        if (*line == '\n') line++;
    }

    /* ── Select best codec per section ───────────────────────────────── */
    for (int s = 0; s < num_sections; s++) {
        media_section_t* sec = &sections[s];
        int best_prio = 10000;
        int best_idx = -1;
        for (int c = 0; c < sec->num_codecs; c++) {
            if (sec->codecs[c].prio < best_prio) {
                best_prio = sec->codecs[c].prio;
                best_idx = c;
            }
        }
        /* Fallback: if no codec matched the preference list (prio == 10000), pick the first one */
        if (best_idx < 0 && sec->num_codecs > 0) {
            best_idx = 0;
        }
        if (best_idx >= 0) {
            sec->selected_pt = sec->codecs[best_idx].pt;
            snprintf(sec->selected_name, sizeof(sec->selected_name),
                     "%s", sec->codecs[best_idx].name);
            av_log(NULL, AV_LOG_INFO, "webrtc_sdp: "
                         "codec_select: section %d (%s): selected %s (pt=%d) from %d offered codecs",
                         s, sec->is_audio ? "audio" : "video",
                         sec->selected_name, sec->selected_pt, sec->num_codecs);

            /* Report to caller */
            if (sec->is_audio && selected_audio_out) {
                snprintf(selected_audio_out, audio_out_len, "%s", sec->selected_name);
            }
            if (!sec->is_audio && selected_video_out) {
                snprintf(selected_video_out, video_out_len, "%s", sec->selected_name);
            }
        } else if (sec->num_codecs == 0) {
            av_log(NULL, AV_LOG_DEBUG, "webrtc_sdp: "
                          "codec_select: section %d has no rtpmap codecs, passing through", s);
        }
    }

    /* ── Pass 2: rewrite SDP ─────────────────────────────────────────── */
    line = sdp;
    cur_section = -1;

    while (*line) {
        /* Bolt optimization: Use SIMD-optimized strchr to find EOL instead of manual loop */
        const char* next_line = strchr(line, '\n');
        if (!next_line) next_line = line + strlen(line);

        size_t len = (size_t)(next_line - line);
        size_t content_len = len;
        if (content_len > 0 && line[content_len - 1] == '\r') content_len--;

        bool keep = true;

        if (strncmp(line, "m=", 2) == 0) {
            cur_section++;

            /* Rewrite the m= line to include only the selected payload type */
            if (cur_section >= 0 && cur_section < num_sections &&
                sections[cur_section].selected_pt >= 0 &&
                sections[cur_section].num_codecs > 1) {

                media_section_t* sec = &sections[cur_section];

                /* Extract the protocol part: "m=<type> <port> <proto>" */
                char mtype[32] = {0};
                int port = 0;
                char proto[64] = {0};

                /* Parse: "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98" */
                const char* after_m = line + 2;
                int n = sscanf(after_m, "%31s %d %63s", mtype, &port, proto);
                if (n == 3) {
                    /* Ensure capacity */
                    size_t needed = strlen(mtype) + strlen(proto) + 64; /* 64 is plenty for ports and formatting */
                    if (out_len + needed > out_cap) {
                        out_cap = out_cap * 2 + needed;
                        char* tmp = realloc(out, out_cap);
                        if (!tmp) { free(out); free_codec_preference(user_prefs); return NULL; }
                        out = tmp;
                    }
                    /* Write rewritten m= line with only selected pt */
                    int written = snprintf(out + out_len, out_cap - out_len,
                                           "m=%s %d %s %d\r\n",
                                           mtype, port, proto, sec->selected_pt);
                    if (written > 0) {
                        out_len += (size_t)written;
                    }
                    keep = false;
                }
            }
        }

        /* Filter out rtpmap/fmtp/rtcp-fb lines for non-selected codecs */
        if (cur_section >= 0 && cur_section < num_sections &&
            sections[cur_section].selected_pt >= 0 &&
            sections[cur_section].num_codecs > 1) {

            int sel_pt = sections[cur_section].selected_pt;
            int line_pt = -1;

            if (strncmp(line, "a=rtpmap:", 9) == 0) {
                sscanf(line + 9, "%d", &line_pt);
            } else if (strncmp(line, "a=fmtp:", 7) == 0) {
                sscanf(line + 7, "%d", &line_pt);
            } else if (strncmp(line, "a=rtcp-fb:", 10) == 0) {
                sscanf(line + 10, "%d", &line_pt);
            }

            if (line_pt >= 0 && line_pt != sel_pt) {
                keep = false;
                av_log(NULL, AV_LOG_DEBUG, "webrtc_sdp: "
                              "codec_select: dropping line for pt=%d (selected pt=%d)",
                              line_pt, sel_pt);
            }
        }

        if (keep) {
            /* Ensure capacity */
            if (out_len + content_len + 4 > out_cap) {
                out_cap = out_cap * 2 + content_len + 4;
                char* tmp = realloc(out, out_cap);
                if (!tmp) { free(out); free_codec_preference(user_prefs); return NULL; }
                out = tmp;
            }
            if (content_len > 0) {
                memcpy(out + out_len, line, content_len);
                out_len += content_len;
            }
            int written = snprintf(out + out_len, out_cap - out_len, "\r\n");
            if (written > 0) {
                out_len += (size_t)written;
            }
        }

        line = next_line;
        if (*line == '\n') line++;
    }

    out[out_len] = '\0';

    free_codec_preference(user_prefs);
    #undef MAX_MEDIA_SECTIONS
    return out;
}

static inline bool webrtc_memstr(const char* haystack, size_t hlen, const char* needle, size_t nlen) {
    if (hlen >= nlen) {
        for (size_t i = 0; i <= hlen - nlen; i++) {
            if (memcmp(haystack + i, needle, nlen) == 0) return true;
        }
    }
    return false;
}

#define WEBRTC_MEMSTR(haystack, hlen, needle) webrtc_memstr(haystack, hlen, needle, sizeof(needle) - 1)

char*
zstr_sdp_filter(const char* sdp)
{
    if (!sdp) return NULL;

    size_t sdp_len = strlen(sdp);
    char* filtered = malloc(sdp_len + 1);
    if (!filtered) return NULL;
    filtered[0] = '\0';

    size_t out_pos = 0;
    const char* line = sdp;
    while (*line) {
        /* Bolt optimization: Use SIMD-optimized strchr to find EOL instead of manual loop */
        const char* next_line = strchr(line, '\n');
        if (!next_line) next_line = line + strlen(line);

        size_t line_len = next_line - line;
        if (*next_line == '\n') {
            line_len++;
        }

        size_t content_len = line_len;
        if (content_len > 0 && line[content_len - 1] == '\n') {
            content_len--;
        }
        if (content_len > 0 && line[content_len - 1] == '\r') {
            content_len--;
        }

        bool keep = true;
        if (content_len >= 9 && memcmp(line, "a=extmap:", 9) == 0) {
            const char* content = line + 9;
            size_t clen = content_len - 9;

            /* NOTE (zff deviation): transport-wide-cc is KEPT — it feeds
             * zstr_webrtc_twcc. Only the rest are stripped. */
            if (WEBRTC_MEMSTR(content, clen, "abs-send-time") ||
                WEBRTC_MEMSTR(content, clen, "goog-playout-delay") ||
                WEBRTC_MEMSTR(content, clen, "playout-delay") ||
                WEBRTC_MEMSTR(content, clen, "video-orientation") ||
                WEBRTC_MEMSTR(content, clen, "ssrc-audio-level")) {
                keep = false;
                av_log(NULL, AV_LOG_INFO, "webrtc_sdp: " "Filtered unsupported SDP extension: %.*s", (int)content_len, line);
            }
        } else if (content_len >= 10 && memcmp(line, "a=rtcp-fb:", 10) == 0) {
            /* NOTE (zff deviation): strip "ccm fir". libdatachannel then
             * uses plain PLI (universally supported, and what our PliHandler
             * detects) instead of FIR, whose request path throws in this
             * libdatachannel build when the sender SSRC is learned late.
             * Peers may still SEND us fir — PliHandler handles FMT=4. */
            const char* content = line + 10;
            size_t clen = content_len - 10;
            if (WEBRTC_MEMSTR(content, clen, "ccm fir")) {
                keep = false;
                av_log(NULL, AV_LOG_INFO, "webrtc_sdp: Filtered ccm-fir RTCP feedback: %.*s", (int)content_len, line);
            }
        }

        if (keep) {
            memcpy(filtered + out_pos, line, line_len);
            out_pos += line_len;
        }

        line = next_line;
        if (*line == '\n') {
            line++;
        }
    }

    filtered[out_pos] = '\0';
    return filtered;
}

char*
zstr_sdp_compat_local(const char* sdp)
{
    if (!sdp) return NULL;

    // Collect all mids
    char mids[16][64];
    int num_mids = 0;

    const char* line = sdp;
    while (*line) {
        /* Bolt optimization: Use SIMD-optimized strchr to find EOL instead of manual loop */
        const char* next_line = strchr(line, '\n');
        if (!next_line) next_line = line + strlen(line);

        size_t len = next_line - line;
        if (len > 0 && line[len - 1] == '\r') len--;

        if (strncmp(line, "a=mid:", 6) == 0 && len > 6) {
            size_t val_len = len - 6;
            if (val_len < 64 && num_mids < 16) {
                memcpy(mids[num_mids], line + 6, val_len);
                mids[num_mids][val_len] = '\0';
                num_mids++;
            }
        }

        line = next_line;
        if (*line == '\n') line++;
    }

    size_t sdp_len = strlen(sdp);
    size_t out_cap = sdp_len + 4096;
    char* out = malloc(out_cap);
    if (!out) return NULL;
    out[0] = '\0';
    size_t out_len = 0;

    bool session_level = true;
    bool has_bundle_group = false;
    int media_section_idx = 0;

    char current_mid[64] = "";
    bool has_rtcp_mux = false;
    bool has_msid = false;
    bool has_ssrc = false;

    #define ENSURE_CAPACITY(needed) do { \
        if (out_len + (needed) + 1 > out_cap) { \
            out_cap = out_cap * 2 + (needed) + 4096; \
            char* tmp = realloc(out, out_cap); \
            if (!tmp) { free(out); return NULL; } \
            out = tmp; \
        } \
    } while(0)

    #define SAFE_APPEND_SNPRINTF(...) do { \
        ENSURE_CAPACITY(512); \
        int written = snprintf(out + out_len, out_cap - out_len, __VA_ARGS__); \
        if (written > 0) { \
            out_len += (size_t)written; \
        } \
    } while (0)

    #define FINISH_MEDIA_SECTION() do { \
        if (!session_level) { \
            if (!has_rtcp_mux) { \
                SAFE_APPEND_SNPRINTF("a=rtcp-mux\r\n"); \
            } \
            if (!has_msid && strlen(current_mid) > 0) { \
                SAFE_APPEND_SNPRINTF("a=msid:zff-stream zff-track-%s\r\n", current_mid); \
            } \
            if (!has_ssrc && strlen(current_mid) > 0) { \
                uint32_t fallback_ssrc = 1000 + media_section_idx; \
                SAFE_APPEND_SNPRINTF("a=ssrc:%u cname:zff-cname\r\n", fallback_ssrc); \
            } \
        } \
    } while(0)

    line = sdp;
    while (*line) {
        /* Bolt optimization: Use SIMD-optimized strchr to find EOL instead of manual loop */
        const char* next_line = strchr(line, '\n');
        if (!next_line) next_line = line + strlen(line);

        size_t len = next_line - line;
        size_t content_len = len;
        if (content_len > 0 && line[content_len - 1] == '\r') content_len--;

        if (strncmp(line, "m=", 2) == 0) {
            media_section_idx++;
            if (session_level) {
                if (!has_bundle_group && num_mids > 0) {
                    SAFE_APPEND_SNPRINTF("a=group:BUNDLE");
                    for (int i = 0; i < num_mids; i++) {
                        SAFE_APPEND_SNPRINTF(" %s", mids[i]);
                    }
                    SAFE_APPEND_SNPRINTF("\r\n");
                }
                session_level = false;
            } else {
                FINISH_MEDIA_SECTION();
            }

            current_mid[0] = '\0';
            has_rtcp_mux = false;
            has_msid = false;
            has_ssrc = false;
        }

        bool skip_original = false;

        if (!session_level) {
            if (strncmp(line, "a=mid:", 6) == 0 && content_len > 6) {
                size_t val_len = content_len - 6;
                if (val_len < 64) {
                    memcpy(current_mid, line + 6, val_len);
                    current_mid[val_len] = '\0';
                }
            } else if (strncmp(line, "a=rtcp-mux", 10) == 0) {
                has_rtcp_mux = true;
            } else if (strncmp(line, "a=msid", 6) == 0) {
                has_msid = true;
            } else if (strncmp(line, "a=ssrc:", 7) == 0) {
                has_ssrc = true;
                char* cname_ptr = strstr((char*)line, "cname:");
                if (cname_ptr) {
                    uint32_t ssrc = 0;
                    sscanf(line + 7, "%u", &ssrc);
                    SAFE_APPEND_SNPRINTF("a=ssrc:%u cname:zff-cname\r\n", ssrc);
                    skip_original = true;
                }
            }
        } else {
            if (strncmp(line, "a=group:BUNDLE", 14) == 0) {
                has_bundle_group = true;
            }
        }

        if (!skip_original) {
            ENSURE_CAPACITY(content_len + 4);
            if (content_len > 0) {
                memcpy(out + out_len, line, content_len);
                out_len += content_len;
            }
            SAFE_APPEND_SNPRINTF("\r\n");
        }

        line = next_line;
        if (*line == '\n') line++;
    }

    FINISH_MEDIA_SECTION();

    #undef SAFE_APPEND_SNPRINTF
    #undef ENSURE_CAPACITY
    #undef FINISH_MEDIA_SECTION
    return out;
}
