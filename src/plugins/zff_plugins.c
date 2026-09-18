/*=============================================================================
    zff_plugins.c — Plugin Registration Implementation
=============================================================================*/
#include "zff/zff_core.h"
#include "zff/plugins/zstr_videotestsrc.h"
#include "zff/plugins/zstr_audiotestsrc.h"

int zff_plugins_register_all(void) {
    zff_register_input_format(&ff_zstr_videotestsrc_demuxer);
    zff_register_input_format(&ff_zstr_audiotestsrc_demuxer);
    return 0;
}

__attribute__((constructor))
static void zff_plugins_auto_init(void) {
    zff_plugins_register_all();
}
