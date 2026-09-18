/*=============================================================================
    zff_plugins.c — Plugin Registration Implementation
=============================================================================*/
#include "zff/zff_core.h"
#include "zff/plugins/zstr_videotestsrc.h"
#include "zff/plugins/zstr_audiotestsrc.h"
#include "zff/plugins/zstr_v4l2.h"
#include "zff/plugins/zstr_alsa.h"
#include "zff/plugins/zstr_v4l2_sink.h"
#include "zff/plugins/zstr_alsa_sink.h"

int zff_plugins_register_all(void) {
    zff_register_input_format(&ff_zstr_videotestsrc_demuxer);
    zff_register_input_format(&ff_zstr_audiotestsrc_demuxer);
    zff_register_input_format(&ff_zstr_v4l2_demuxer);
    zff_register_input_format(&ff_zstr_alsa_demuxer);
    zff_register_output_format(&ff_zstr_v4l2_sink_muxer.p);
    zff_register_output_format(&ff_zstr_alsa_sink_muxer.p);
    return 0;
}

__attribute__((constructor))
static void zff_plugins_auto_init(void) {
    zff_plugins_register_all();
}
