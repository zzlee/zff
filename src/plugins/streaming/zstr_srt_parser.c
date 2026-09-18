/*=============================================================================
    zstr_srt_parser.c — SRT Subtitle File/Memory Parser
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include "zff/plugins/zstr_srt_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>

struct zstr_srt_parser {
    zstr_srt_subtitle_entry_t *entries;
    int count;
    int capacity;
};

static int64_t parse_srt_timestamp_ms(const char *s)
{
    int hh = 0, mm = 0, ss = 0, ms = 0;
    /* Try with comma first (standard SRT), then dot */
    if (sscanf(s, "%d:%d:%d,%d", &hh, &mm, &ss, &ms) == 4 ||
        sscanf(s, "%d:%d:%d.%d", &hh, &mm, &ss, &ms) == 4) {
        return ((int64_t)hh * 3600 + mm * 60 + ss) * 1000 + ms;
    }
    return 0;
}

static char* trim_line(char *line)
{
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') return line;
    char *end = line + strlen(line) - 1;
    while (end > line && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return line;
}

static int add_entry(zstr_srt_parser_t *p, int index, int64_t start_ms, int64_t duration_ms, const char *text)
{
    if (p->count >= p->capacity) {
        int new_cap = p->capacity == 0 ? 16 : p->capacity * 2;
        zstr_srt_subtitle_entry_t *new_entries = realloc(p->entries, new_cap * sizeof(*new_entries));
        if (!new_entries) return AVERROR(ENOMEM);
        p->entries = new_entries;
        p->capacity = new_cap;
    }

    p->entries[p->count].index = index;
    p->entries[p->count].start_ms = start_ms;
    p->entries[p->count].duration_ms = duration_ms;
    p->entries[p->count].text = strdup(text ? text : "");
    if (!p->entries[p->count].text) return AVERROR(ENOMEM);

    p->count++;
    return 0;
}

static int parse_lines(zstr_srt_parser_t *p, char **lines, int total_lines)
{
    int line_idx = 0;
    while (line_idx < total_lines) {
        char *line = trim_line(lines[line_idx++]);
        if (line[0] == '\0') continue;

        /* Subtitle index */
        int sub_idx = atoi(line);

        /* Timestamp line */
        if (line_idx >= total_lines) break;
        char *time_line = trim_line(lines[line_idx++]);
        char *arrow = strstr(time_line, "-->");
        if (!arrow) continue;

        char start_str[64] = {0};
        char end_str[64] = {0};
        if (sscanf(time_line, "%63s --> %63s", start_str, end_str) < 2) continue;

        int64_t start_ms = parse_srt_timestamp_ms(start_str);
        int64_t end_ms = parse_srt_timestamp_ms(end_str);
        int64_t dur_ms = (end_ms > start_ms) ? (end_ms - start_ms) : 0;

        /* Multi-line subtitle text */
        char text_buf[2048] = {0};
        size_t text_len = 0;

        while (line_idx < total_lines) {
            char *t_line = trim_line(lines[line_idx]);
            if (t_line[0] == '\0') {
                line_idx++;
                break; /* Blank line terminates entry */
            }
            /* Also safeguard if next line is index followed by arrow */
            if (line_idx + 1 < total_lines && isdigit((unsigned char)t_line[0]) && strstr(lines[line_idx + 1], "-->")) {
                break;
            }
            line_idx++;

            size_t t_len = strlen(t_line);
            if (text_len + t_len + 2 < sizeof(text_buf)) {
                if (text_len > 0) {
                    text_buf[text_len++] = '\n';
                }
                memcpy(text_buf + text_len, t_line, t_len);
                text_len += t_len;
                text_buf[text_len] = '\0';
            }
        }

        if (text_len > 0) {
            add_entry(p, sub_idx, start_ms, dur_ms, text_buf);
        }
    }

    return 0;
}

zstr_srt_parser_t* zstr_srt_parser_create_from_memory(const char *data, size_t len)
{
    if (!data || len == 0) return NULL;

    zstr_srt_parser_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    /* Copy buffer and split by lines, preserving empty lines */
    char *copy = malloc(len + 2);
    if (!copy) {
        free(p);
        return NULL;
    }
    memcpy(copy, data, len);
    copy[len] = '\0';

    int max_lines = 16;
    for (size_t i = 0; i < len; i++) {
        if (copy[i] == '\n') max_lines++;
    }
    max_lines = max_lines * 2 + 16;

    char **lines = malloc(max_lines * sizeof(char*));
    if (!lines) {
        free(copy);
        free(p);
        return NULL;
    }

    int n_lines = 0;
    char *cur = copy;
    while (*cur) {
        char *line_start = cur;
        while (*cur && *cur != '\r' && *cur != '\n') cur++;
        if (*cur == '\r') {
            *cur++ = '\0';
            if (*cur == '\n') *cur++ = '\0';
        } else if (*cur == '\n') {
            *cur++ = '\0';
        }
        if (n_lines < max_lines) {
            lines[n_lines++] = line_start;
        }
    }

    parse_lines(p, lines, n_lines);

    free(lines);
    free(copy);
    return p;
}

zstr_srt_parser_t* zstr_srt_parser_create_from_file(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);
    buf[read_bytes] = '\0';

    zstr_srt_parser_t *p = zstr_srt_parser_create_from_memory(buf, read_bytes);
    free(buf);
    return p;
}

int zstr_srt_parser_get_count(const zstr_srt_parser_t *p)
{
    return p ? p->count : 0;
}

int zstr_srt_parser_get_entry(const zstr_srt_parser_t *p, int index,
                              int64_t *start_ms, int64_t *duration_ms,
                              const char **text)
{
    if (!p || index < 0 || index >= p->count) return AVERROR(EINVAL);
    if (start_ms) *start_ms = p->entries[index].start_ms;
    if (duration_ms) *duration_ms = p->entries[index].duration_ms;
    if (text) *text = p->entries[index].text;
    return 0;
}

const char* zstr_srt_parser_find_at_time(const zstr_srt_parser_t *p, int64_t time_ms)
{
    if (!p || p->count == 0) return NULL;

    for (int i = 0; i < p->count; i++) {
        int64_t start = p->entries[i].start_ms;
        int64_t end = start + p->entries[i].duration_ms;
        if (time_ms >= start && time_ms < end) {
            return p->entries[i].text;
        }
    }
    return NULL;
}

int zstr_srt_parser_apply_to_overlay(const zstr_srt_parser_t *p,
                                     zstr_text_overlay_t *overlay,
                                     int64_t current_pts,
                                     AVRational time_base)
{
    if (!p || !overlay) return AVERROR(EINVAL);

    int64_t time_ms = av_rescale_q(current_pts, time_base, (AVRational){1, 1000});
    for (int i = 0; i < p->count; i++) {
        int64_t start = p->entries[i].start_ms;
        int64_t end = start + p->entries[i].duration_ms;
        if (time_ms >= start && time_ms < end) {
            int64_t start_pts = av_rescale_q(start, (AVRational){1, 1000}, time_base);
            int64_t dur_pts = av_rescale_q(p->entries[i].duration_ms, (AVRational){1, 1000}, time_base);
            return zstr_text_overlay_set_subtitle(overlay, p->entries[i].text,
                                                 start_pts, dur_pts, time_base);
        }
    }

    return zstr_text_overlay_set_subtitle(overlay, NULL, 0, 0, time_base);
}

void zstr_srt_parser_free(zstr_srt_parser_t **pp)
{
    if (!pp || !*pp) return;
    zstr_srt_parser_t *p = *pp;

    for (int i = 0; i < p->count; i++) {
        free(p->entries[i].text);
    }
    free(p->entries);
    free(p);
    *pp = NULL;
}
