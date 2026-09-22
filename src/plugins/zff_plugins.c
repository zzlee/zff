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
#include "zff/plugins/zstr_glsink.h"
#include "zff/plugins/zstr_x11sink.h"
#include "zff/plugins/zstr_gl_comp.h"
#include "zff/plugins/zstr_net.h"
#include "zff/plugins/zstr_rtsp_server.h"
#include "zff/plugins/zstr_hls_sink.h"
#include "zff/plugins/zstr_st2110.h"
#include "zff/plugins/zstr_srt.h"
#ifdef HAS_WEBRTC
#include "zff/plugins/zstr_webrtc.h"
#endif

int zff_plugins_register_all(void) {
    zff_register_input_format(&ff_zstr_videotestsrc_demuxer);
    zff_register_input_format(&ff_zstr_audiotestsrc_demuxer);
    zff_register_input_format(&ff_zstr_v4l2_demuxer);
    zff_register_input_format(&ff_zstr_alsa_demuxer);
    zff_register_input_format(&ff_zstr_net_source_demuxer);
    zff_register_input_format(&ff_zstr_st2110_demuxer);
    zff_register_input_format(&ff_zstr_srt_source_demuxer);
    zff_register_output_format(&ff_zstr_v4l2_sink_muxer.p);
    zff_register_output_format(&ff_zstr_alsa_sink_muxer.p);
    zff_register_output_format(&ff_zstr_glsink_muxer.p);
    zff_register_output_format(&ff_zstr_x11sink_muxer.p);
    zff_register_output_format(&ff_zstr_gl_comp_muxer.p);
    zff_register_output_format(&ff_zstr_net_sink_muxer.p);
    zff_register_output_format(&ff_zstr_rtspserver_muxer.p);
    zff_register_output_format(&ff_zstr_hls_sink_muxer.p);
    zff_register_output_format(&ff_zstr_st2110_muxer.p);
    zff_register_output_format(&ff_zstr_srt_sink_muxer.p);
#ifdef HAS_WEBRTC
    zff_register_input_format(&ff_zstr_webrtc_demuxer);
    zff_register_output_format(&ff_zstr_webrtc_muxer.p);
#endif
    return 0;
}

__attribute__((constructor))
static void zff_plugins_auto_init(void) {
    zff_plugins_register_all();
}
