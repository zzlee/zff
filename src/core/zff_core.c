/*=============================================================================
    zff_core.c — zff Core Global Registration & Runtime Initializers
=============================================================================*/
#include "zff/zff_core.h"
#include <string.h>

#define MAX_FORMATS 128
static const AVInputFormat *s_registered_formats[MAX_FORMATS];
static int s_num_formats = 0;

int zff_register_input_format(const AVInputFormat *fmt) {
    if (!fmt) return -1;
    for (int i = 0; i < s_num_formats; i++) {
        if (s_registered_formats[i] == fmt) return 0; // already registered
    }
    if (s_num_formats >= MAX_FORMATS) return -1;
    s_registered_formats[s_num_formats++] = fmt;
    return 0;
}

const AVInputFormat* zff_find_input_format(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < s_num_formats; i++) {
        if (s_registered_formats[i]->name && strcmp(s_registered_formats[i]->name, name) == 0) {
            return s_registered_formats[i];
        }
    }
    return av_find_input_format(name);
}

static const AVOutputFormat *s_registered_out_formats[MAX_FORMATS];
static int s_num_out_formats = 0;

int zff_register_output_format(const AVOutputFormat *fmt) {
    if (!fmt) return -1;
    for (int i = 0; i < s_num_out_formats; i++) {
        if (s_registered_out_formats[i] == fmt) return 0; // already registered
    }
    if (s_num_out_formats >= MAX_FORMATS) return -1;
    s_registered_out_formats[s_num_out_formats++] = fmt;
    return 0;
}

const AVOutputFormat* zff_find_output_format(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < s_num_out_formats; i++) {
        if (s_registered_out_formats[i]->name && strcmp(s_registered_out_formats[i]->name, name) == 0) {
            return s_registered_out_formats[i];
        }
    }
    return av_guess_format(name, NULL, NULL);
}
