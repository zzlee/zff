/*=============================================================================
    zstr_glsink.c — OpenGL/GLX Video Display Sink Device (AVOutputFormat)
=============================================================================*/
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES 1

#include "zff/plugins/zstr_glsink.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

typedef struct GLSinkContext {
    const AVClass *av_class;

    char *display;
    char *window_title;
    int fullscreen;
    int vsync;
    int is_mock;

    int width;
    int height;
    enum AVPixelFormat av_pix_fmt;

    int null_mode;
    Display *x_display;
    Window x_window;
    GLXContext gl_context;
    Atom wm_delete_window;

    GLuint tex_y;
    GLuint tex_u;
    GLuint tex_v;
    GLuint tex_uv;
    GLuint tex_rgb;

    GLuint prog_yuv420p;
    GLuint prog_nv12;
    GLuint prog_rgb;

    int64_t frames_rendered;
} GLSinkContext;

#define OFFSET(x) offsetof(GLSinkContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_glsink_options[] = {
    { "display",      "X11 display string (e.g. :0, :99)", OFFSET(display),      AV_OPT_TYPE_STRING, { .str = NULL },        0, 0, ENC },
    { "window_title", "Window title",                     OFFSET(window_title), AV_OPT_TYPE_STRING, { .str = "zff glsink" }, 0, 0, ENC },
    { "fullscreen",   "Fullscreen mode",                  OFFSET(fullscreen),   AV_OPT_TYPE_BOOL,   { .i64 = 0 },            0, 1, ENC },
    { "vsync",        "Enable vsync",                     OFFSET(vsync),        AV_OPT_TYPE_BOOL,   { .i64 = 1 },            0, 1, ENC },
    { "is_mock",      "Force null-mode fake sink",        OFFSET(is_mock),      AV_OPT_TYPE_BOOL,   { .i64 = 0 },            0, 1, ENC },
    { NULL }
};

static const AVClass zstr_glsink_class = {
    .class_name = "zstr_glsink",
    .item_name  = av_default_item_name,
    .option     = zstr_glsink_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const char *vs_source =
    "#version 120\n"
    "void main() {\n"
    "    gl_Position = ftransform();\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "}\n";

static const char *fs_yuv420p_source =
    "#version 120\n"
    "uniform sampler2D y_tex;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D v_tex;\n"
    "void main() {\n"
    "    vec2 tc = gl_TexCoord[0].st;\n"
    "    float y = texture2D(y_tex, tc).r;\n"
    "    float u = texture2D(u_tex, tc).r - 0.5;\n"
    "    float v = texture2D(v_tex, tc).r - 0.5;\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.344136 * u - 0.714136 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    gl_FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);\n"
    "}\n";

static const char *fs_rgb_source =
    "#version 120\n"
    "uniform sampler2D rgb_tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(rgb_tex, gl_TexCoord[0].st);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static int init_glx(GLSinkContext *ctx) {
    const char *disp_name = (ctx->display && ctx->display[0]) ? ctx->display : getenv("DISPLAY");
    if (!disp_name || !disp_name[0]) {
        ctx->null_mode = 1;
        return 0;
    }

    ctx->x_display = XOpenDisplay(disp_name);
    if (!ctx->x_display) {
        ctx->null_mode = 1;
        return 0;
    }

    int screen = DefaultScreen(ctx->x_display);
    static int visual_attribs[] = {
        GLX_RGBA,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER,
        None
    };

    XVisualInfo *vi = glXChooseVisual(ctx->x_display, screen, visual_attribs);
    if (!vi) {
        XCloseDisplay(ctx->x_display);
        ctx->x_display = NULL;
        ctx->null_mode = 1;
        return 0;
    }

    Colormap cmap = XCreateColormap(ctx->x_display, RootWindow(ctx->x_display, vi->screen),
                                   vi->visual, AllocNone);
    XSetWindowAttributes swa;
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;

    ctx->x_window = XCreateWindow(ctx->x_display, RootWindow(ctx->x_display, vi->screen),
                                 0, 0, ctx->width, ctx->height, 0, vi->depth,
                                 InputOutput, vi->visual,
                                 CWColormap | CWEventMask, &swa);

    XStoreName(ctx->x_display, ctx->x_window, ctx->window_title ? ctx->window_title : "zff glsink");
    ctx->wm_delete_window = XInternAtom(ctx->x_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ctx->x_display, ctx->x_window, &ctx->wm_delete_window, 1);
    XMapWindow(ctx->x_display, ctx->x_window);

    ctx->gl_context = glXCreateContext(ctx->x_display, vi, NULL, GL_TRUE);
    XFree(vi);
    if (!ctx->gl_context) {
        XDestroyWindow(ctx->x_display, ctx->x_window);
        XCloseDisplay(ctx->x_display);
        ctx->x_display = NULL;
        ctx->null_mode = 1;
        return 0;
    }

    glXMakeCurrent(ctx->x_display, ctx->x_window, ctx->gl_context);

    /* Compile shaders */
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_source);
    GLuint fs_yuv = compile_shader(GL_FRAGMENT_SHADER, fs_yuv420p_source);
    GLuint fs_rgb = compile_shader(GL_FRAGMENT_SHADER, fs_rgb_source);

    if (vs && fs_yuv) {
        ctx->prog_yuv420p = link_program(vs, fs_yuv);
    }
    if (vs && fs_rgb) {
        ctx->prog_rgb = link_program(vs, fs_rgb);
    }

    if (vs) glDeleteShader(vs);
    if (fs_yuv) glDeleteShader(fs_yuv);
    if (fs_rgb) glDeleteShader(fs_rgb);

    /* Textures */
    glGenTextures(1, &ctx->tex_y);
    glGenTextures(1, &ctx->tex_u);
    glGenTextures(1, &ctx->tex_v);
    glGenTextures(1, &ctx->tex_rgb);

    glBindTexture(GL_TEXTURE_2D, ctx->tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, ctx->width, ctx->height, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

    glBindTexture(GL_TEXTURE_2D, ctx->tex_u);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, ctx->width / 2, ctx->height / 2, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

    glBindTexture(GL_TEXTURE_2D, ctx->tex_v);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, ctx->width / 2, ctx->height / 2, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

    glBindTexture(GL_TEXTURE_2D, ctx->tex_rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, ctx->width, ctx->height, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);

    ctx->null_mode = 0;
    return 0;
}

static int glsink_write_header(AVFormatContext *s) {
    GLSinkContext *ctx = s->priv_data;

    if (s->nb_streams < 1 || s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        av_log(s, AV_LOG_ERROR, "zstr_glsink requires at least one video stream\n");
        return AVERROR(EINVAL);
    }

    AVCodecParameters *par = s->streams[0]->codecpar;
    ctx->width = par->width > 0 ? par->width : 640;
    ctx->height = par->height > 0 ? par->height : 480;
    ctx->av_pix_fmt = par->format != AV_PIX_FMT_NONE ? par->format : AV_PIX_FMT_YUV420P;
    ctx->frames_rendered = 0;

    if (ctx->is_mock) {
        ctx->null_mode = 1;
        return 0;
    }

    return init_glx(ctx);
}

static int glsink_write_packet(AVFormatContext *s, AVPacket *pkt) {
    GLSinkContext *ctx = s->priv_data;
    if (!pkt || !pkt->data) return 0;

    if (ctx->null_mode || !ctx->x_display) {
        ctx->frames_rendered++;
        return 0;
    }

    /* Pump X11 events */
    while (XPending(ctx->x_display)) {
        XEvent ev;
        XNextEvent(ctx->x_display, &ev);
    }

    glXMakeCurrent(ctx->x_display, ctx->x_window, ctx->gl_context);
    glViewport(0, 0, ctx->width, ctx->height);
    glClear(GL_COLOR_BUFFER_BIT);

    int w = ctx->width;
    int h = ctx->height;

    if (ctx->av_pix_fmt == AV_PIX_FMT_RGB24 && ctx->prog_rgb) {
        glUseProgram(ctx->prog_rgb);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx->tex_rgb);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pkt->data);
        glUniform1i(glGetUniformLocation(ctx->prog_rgb, "rgb_tex"), 0);
    } else if (ctx->prog_yuv420p) {
        /* Default YUV420P */
        const uint8_t *y_plane = pkt->data;
        const uint8_t *u_plane = y_plane + (w * h);
        const uint8_t *v_plane = u_plane + (w * h / 4);

        glUseProgram(ctx->prog_yuv420p);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx->tex_y);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, y_plane);
        glUniform1i(glGetUniformLocation(ctx->prog_yuv420p, "y_tex"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx->tex_u);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w / 2, h / 2, GL_LUMINANCE, GL_UNSIGNED_BYTE, u_plane);
        glUniform1i(glGetUniformLocation(ctx->prog_yuv420p, "u_tex"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ctx->tex_v);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w / 2, h / 2, GL_LUMINANCE, GL_UNSIGNED_BYTE, v_plane);
        glUniform1i(glGetUniformLocation(ctx->prog_yuv420p, "v_tex"), 2);
    }

    /* Render full screen quad */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
    glEnd();

    glXSwapBuffers(ctx->x_display, ctx->x_window);
    ctx->frames_rendered++;
    return 0;
}

static int glsink_write_trailer(AVFormatContext *s) {
    GLSinkContext *ctx = s->priv_data;

    if (!ctx->null_mode && ctx->x_display) {
        glXMakeCurrent(ctx->x_display, ctx->x_window, ctx->gl_context);

        if (ctx->tex_y) glDeleteTextures(1, &ctx->tex_y);
        if (ctx->tex_u) glDeleteTextures(1, &ctx->tex_u);
        if (ctx->tex_v) glDeleteTextures(1, &ctx->tex_v);
        if (ctx->tex_rgb) glDeleteTextures(1, &ctx->tex_rgb);

        if (ctx->prog_yuv420p) glDeleteProgram(ctx->prog_yuv420p);
        if (ctx->prog_rgb) glDeleteProgram(ctx->prog_rgb);

        glXMakeCurrent(ctx->x_display, None, NULL);
        if (ctx->gl_context) glXDestroyContext(ctx->x_display, ctx->gl_context);
        if (ctx->x_window) XDestroyWindow(ctx->x_display, ctx->x_window);
        XCloseDisplay(ctx->x_display);
        ctx->x_display = NULL;
    }

    return 0;
}

const FFOutputFormat ff_zstr_glsink_muxer = {
    .p = {
        .name           = "zstr_glsink",
        .long_name      = "zff OpenGL/GLX Display Sink Device",
        .video_codec    = AV_CODEC_ID_RAWVIDEO,
        .audio_codec    = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE,
        .priv_class     = &zstr_glsink_class,
    },
    .priv_data_size = sizeof(GLSinkContext),
    .write_header   = glsink_write_header,
    .write_packet   = glsink_write_packet,
    .write_trailer  = glsink_write_trailer,
};
