/*=============================================================================
    zstr_gl_comp.c — OpenGL Video Compositor & Multi-Stream Sink Device
=============================================================================*/
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES 1

#include "zff/plugins/zstr_gl_comp.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>

typedef struct {
    zstr_gl_comp_layer_cfg_t cfg;
    AVFrame *latest_frame;
    GLuint tex_y;
    GLuint tex_u;
    GLuint tex_v;
    GLuint tex_uv;
    GLuint tex_rgb;
    int tex_w;
    int tex_h;
    bool configured;
} comp_layer_t;

struct zstr_gl_comp {
    int canvas_width;
    int canvas_height;
    char *window_title;
    char *display_name;
    uint32_t bg_color;
    bool fullscreen;
    bool vsync;
    bool is_mock;

    int null_mode;
    Display *x_display;
    Window x_window;
    GLXContext gl_context;
    Atom wm_delete_window;

    GLuint prog_yuv420p;
    GLuint prog_nv12;
    GLuint prog_rgb;

    comp_layer_t layers[ZSTR_GL_COMP_MAX_LAYERS];
    int max_layers;

    uint8_t *sw_canvas; /* RGBA canvas buffer for capture & software fallback */

    pthread_mutex_t lock;
    int64_t frames_rendered;
};

/* ---------------------------------------------------------------------------
 * Shader Sources
 * --------------------------------------------------------------------------- */
static const char *comp_vs_source =
    "#version 120\n"
    "void main() {\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "}\n";

static const char *comp_fs_yuv420p_source =
    "#version 120\n"
    "uniform sampler2D y_tex;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D v_tex;\n"
    "uniform float alpha;\n"
    "void main() {\n"
    "    vec2 tc = gl_TexCoord[0].st;\n"
    "    float y = texture2D(y_tex, tc).r;\n"
    "    float u = texture2D(u_tex, tc).r - 0.5;\n"
    "    float v = texture2D(v_tex, tc).r - 0.5;\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.344136 * u - 0.714136 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    gl_FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), alpha);\n"
    "}\n";

static const char *comp_fs_rgb_source =
    "#version 120\n"
    "uniform sampler2D rgb_tex;\n"
    "uniform float alpha;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(texture2D(rgb_tex, gl_TexCoord[0].st).rgb, alpha);\n"
    "}\n";

static GLuint comp_compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    if (!s) return 0;
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

static GLuint comp_link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    if (!p) return 0;
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

/* ---------------------------------------------------------------------------
 * GL & Window Setup
 * --------------------------------------------------------------------------- */
static int init_x11_glx(zstr_gl_comp_t *s) {
    if (s->is_mock) {
        s->null_mode = 1;
        return 0;
    }

    const char *disp_str = s->display_name;
    if (!disp_str || disp_str[0] == '\0') {
        disp_str = getenv("DISPLAY");
    }
    if (!disp_str || disp_str[0] == '\0') {
        s->null_mode = 1;
        return 0;
    }

    s->x_display = XOpenDisplay(disp_str);
    if (!s->x_display) {
        s->null_mode = 1;
        return 0;
    }

    static int visual_attribs[] = {
        GLX_RGBA,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER,
        None
    };

    int screen = DefaultScreen(s->x_display);
    XVisualInfo *vi = glXChooseVisual(s->x_display, screen, visual_attribs);
    if (!vi) {
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
        s->null_mode = 1;
        return 0;
    }

    Colormap cmap = XCreateColormap(s->x_display, RootWindow(s->x_display, vi->screen), vi->visual, AllocNone);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;

    s->x_window = XCreateWindow(
        s->x_display, RootWindow(s->x_display, vi->screen),
        0, 0, s->canvas_width, s->canvas_height, 0,
        vi->depth, InputOutput, vi->visual,
        CWColormap | CWEventMask, &swa
    );

    s->wm_delete_window = XInternAtom(s->x_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s->x_display, s->x_window, &s->wm_delete_window, 1);
    XStoreName(s->x_display, s->x_window, s->window_title ? s->window_title : "zff gl_comp");
    XMapWindow(s->x_display, s->x_window);

    s->gl_context = glXCreateContext(s->x_display, vi, NULL, GL_TRUE);
    XFree(vi);
    if (!s->gl_context) {
        XDestroyWindow(s->x_display, s->x_window);
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
        s->null_mode = 1;
        return 0;
    }

    if (!glXMakeCurrent(s->x_display, s->x_window, s->gl_context)) {
        glXDestroyContext(s->x_display, s->gl_context);
        XDestroyWindow(s->x_display, s->x_window);
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
        s->null_mode = 1;
        return 0;
    }

    GLuint vs = comp_compile_shader(GL_VERTEX_SHADER, comp_vs_source);
    GLuint fs_yuv = comp_compile_shader(GL_FRAGMENT_SHADER, comp_fs_yuv420p_source);
    GLuint fs_rgb = comp_compile_shader(GL_FRAGMENT_SHADER, comp_fs_rgb_source);

    if (vs && fs_yuv) s->prog_yuv420p = comp_link_program(vs, fs_yuv);
    if (vs && fs_rgb) s->prog_rgb = comp_link_program(vs, fs_rgb);

    if (vs) glDeleteShader(vs);
    if (fs_yuv) glDeleteShader(fs_yuv);
    if (fs_rgb) glDeleteShader(fs_rgb);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    s->null_mode = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */
zstr_gl_comp_t* zstr_gl_comp_create(const zstr_gl_comp_config_t *cfg)
{
    zstr_gl_comp_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->canvas_width = (cfg && cfg->canvas_width > 0) ? cfg->canvas_width : 1920;
    c->canvas_height = (cfg && cfg->canvas_height > 0) ? cfg->canvas_height : 1080;
    c->window_title = strdup((cfg && cfg->window_title) ? cfg->window_title : "zff gl_comp");
    if (cfg && cfg->display) c->display_name = strdup(cfg->display);
    c->bg_color = (cfg && cfg->bg_color) ? cfg->bg_color : 0x000000FF;
    c->fullscreen = cfg ? cfg->fullscreen : false;
    c->vsync = cfg ? cfg->vsync : true;
    c->is_mock = cfg ? cfg->is_mock : false;
    c->max_layers = ZSTR_GL_COMP_MAX_LAYERS;

    c->sw_canvas = malloc(c->canvas_width * c->canvas_height * 4);
    pthread_mutex_init(&c->lock, NULL);

    /* Initialize default layer configurations */
    for (int i = 0; i < ZSTR_GL_COMP_MAX_LAYERS; i++) {
        c->layers[i].cfg.x = 0;
        c->layers[i].cfg.y = 0;
        c->layers[i].cfg.width = 0;
        c->layers[i].cfg.height = 0;
        c->layers[i].cfg.z_order = i;
        c->layers[i].cfg.alpha = 1.0f;
        c->layers[i].cfg.visible = true;
        c->layers[i].cfg.border_width = 0;
        c->layers[i].cfg.border_color = 0xFFFFFFFF;
    }

    init_x11_glx(c);
    return c;
}

zstr_gl_comp_t* zstr_gl_comp_alloc(const char *opt_string)
{
    zstr_gl_comp_config_t cfg = {
        .canvas_width = 1280,
        .canvas_height = 720,
        .window_title = "zff gl_comp",
        .display = NULL,
        .bg_color = 0x000000FF,
        .fullscreen = false,
        .vsync = true,
        .is_mock = false
    };

    if (opt_string && opt_string[0] != '\0') {
        char *copy = strdup(opt_string);
        if (copy) {
            char *tok = strtok(copy, ":,");
            while (tok) {
                char *eq = strchr(tok, '=');
                if (eq) {
                    *eq = '\0';
                    const char *k = tok;
                    const char *v = eq + 1;
                    if (strcmp(k, "w") == 0 || strcmp(k, "width") == 0) {
                        cfg.canvas_width = atoi(v);
                    } else if (strcmp(k, "h") == 0 || strcmp(k, "height") == 0) {
                        cfg.canvas_height = atoi(v);
                    } else if (strcmp(k, "title") == 0) {
                        cfg.window_title = v;
                    } else if (strcmp(k, "display") == 0) {
                        cfg.display = v;
                    } else if (strcmp(k, "bg") == 0 || strcmp(k, "bgcolor") == 0) {
                        cfg.bg_color = (uint32_t)strtoul(v, NULL, 0);
                    } else if (strcmp(k, "mock") == 0 || strcmp(k, "is_mock") == 0) {
                        cfg.is_mock = (atoi(v) != 0);
                    }
                }
                tok = strtok(NULL, ":,");
            }
            zstr_gl_comp_t *res = zstr_gl_comp_create(&cfg);
            free(copy);
            return res;
        }
    }

    return zstr_gl_comp_create(&cfg);
}

int zstr_gl_comp_configure_layer(zstr_gl_comp_t *c, int layer_idx, const zstr_gl_comp_layer_cfg_t *cfg)
{
    if (!c || layer_idx < 0 || layer_idx >= c->max_layers || !cfg)
        return AVERROR(EINVAL);

    pthread_mutex_lock(&c->lock);
    c->layers[layer_idx].cfg = *cfg;
    c->layers[layer_idx].configured = true;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

int zstr_gl_comp_set_layer_frame(zstr_gl_comp_t *c, int layer_idx, const AVFrame *frame)
{
    if (!c || layer_idx < 0 || layer_idx >= c->max_layers)
        return AVERROR(EINVAL);

    pthread_mutex_lock(&c->lock);
    if (c->layers[layer_idx].latest_frame) {
        av_frame_free(&c->layers[layer_idx].latest_frame);
    }
    if (frame) {
        c->layers[layer_idx].latest_frame = av_frame_clone(frame);
    }
    pthread_mutex_unlock(&c->lock);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Software Composition Helper
 * --------------------------------------------------------------------------- */
static void software_composite(zstr_gl_comp_t *c)
{
    if (!c->sw_canvas) return;

    /* Fill background */
    uint8_t bg_r = (c->bg_color >> 24) & 0xFF;
    uint8_t bg_g = (c->bg_color >> 16) & 0xFF;
    uint8_t bg_b = (c->bg_color >> 8) & 0xFF;
    uint8_t bg_a = c->bg_color & 0xFF;

    uint32_t *p32 = (uint32_t*)c->sw_canvas;
    uint32_t bg_pixel = (bg_a << 24) | (bg_b << 16) | (bg_g << 8) | bg_r;
    int total_pixels = c->canvas_width * c->canvas_height;
    for (int i = 0; i < total_pixels; i++) {
        p32[i] = bg_pixel;
    }

    /* Sort layers by z_order */
    int order[ZSTR_GL_COMP_MAX_LAYERS];
    for (int i = 0; i < c->max_layers; i++) order[i] = i;
    for (int i = 0; i < c->max_layers - 1; i++) {
        for (int j = i + 1; j < c->max_layers; j++) {
            if (c->layers[order[j]].cfg.z_order < c->layers[order[i]].cfg.z_order) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    /* Render sorted layers */
    for (int idx = 0; idx < c->max_layers; idx++) {
        int l_idx = order[idx];
        comp_layer_t *layer = &c->layers[l_idx];
        if (!layer->cfg.visible || !layer->latest_frame || layer->cfg.alpha <= 0.0f)
            continue;

        AVFrame *f = layer->latest_frame;
        int dst_w = layer->cfg.width > 0 ? layer->cfg.width : f->width;
        int dst_h = layer->cfg.height > 0 ? layer->cfg.height : f->height;
        int dst_x = layer->cfg.x;
        int dst_y = layer->cfg.y;
        float alpha = layer->cfg.alpha;

        /* Convert layer frame to RGBA temp buffer */
        struct SwsContext *sws = sws_getContext(
            f->width, f->height, f->format,
            dst_w, dst_h, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, NULL, NULL, NULL
        );
        if (!sws) continue;

        uint8_t *tmp_rgba = malloc(dst_w * dst_h * 4);
        uint8_t *dst_data[4] = { tmp_rgba, NULL, NULL, NULL };
        int dst_linesize[4] = { dst_w * 4, 0, 0, 0 };
        sws_scale(sws, (const uint8_t * const*)f->data, f->linesize, 0, f->height, dst_data, dst_linesize);
        sws_freeContext(sws);

        /* Draw optional border */
        if (layer->cfg.border_width > 0) {
            int bw = layer->cfg.border_width;
            uint32_t bc = layer->cfg.border_color;
            uint8_t br = (bc >> 24) & 0xFF;
            uint8_t bg = (bc >> 16) & 0xFF;
            uint8_t bb = (bc >> 8) & 0xFF;
            uint8_t ba = bc & 0xFF;

            int bx1 = dst_x - bw;
            int by1 = dst_y - bw;
            int bx2 = dst_x + dst_w + bw;
            int by2 = dst_y + dst_h + bw;

            for (int y = by1; y < by2; y++) {
                if (y < 0 || y >= c->canvas_height) continue;
                for (int x = bx1; x < bx2; x++) {
                    if (x < 0 || x >= c->canvas_width) continue;
                    if (x < dst_x || x >= dst_x + dst_w || y < dst_y || y >= dst_y + dst_h) {
                        uint8_t *dst = c->sw_canvas + (y * c->canvas_width + x) * 4;
                        dst[0] = (uint8_t)((br * ba + dst[0] * (255 - ba)) / 255);
                        dst[1] = (uint8_t)((bg * ba + dst[1] * (255 - ba)) / 255);
                        dst[2] = (uint8_t)((bb * ba + dst[2] * (255 - ba)) / 255);
                        dst[3] = (uint8_t)(ba + dst[3] * (255 - ba) / 255);
                    }
                }
            }
        }

        /* Alpha blend scaled layer into canvas */
        uint8_t ualpha = (uint8_t)(alpha >= 1.0f ? 255 : (alpha * 255.0f));
        for (int row = 0; row < dst_h; row++) {
            int cy = dst_y + row;
            if (cy < 0 || cy >= c->canvas_height) continue;
            for (int col = 0; col < dst_w; col++) {
                int cx = dst_x + col;
                if (cx < 0 || cx >= c->canvas_width) continue;

                uint8_t *src = tmp_rgba + (row * dst_w + col) * 4;
                uint8_t *dst = c->sw_canvas + (cy * c->canvas_width + cx) * 4;

                uint8_t eff_a = (uint8_t)((src[3] * ualpha) / 255);
                if (eff_a == 0) continue;

                dst[0] = (uint8_t)((src[0] * eff_a + dst[0] * (255 - eff_a)) / 255);
                dst[1] = (uint8_t)((src[1] * eff_a + dst[1] * (255 - eff_a)) / 255);
                dst[2] = (uint8_t)((src[2] * eff_a + dst[2] * (255 - eff_a)) / 255);
                dst[3] = (uint8_t)(eff_a + dst[3] * (255 - eff_a) / 255);
            }
        }
        free(tmp_rgba);
    }
}

/* ---------------------------------------------------------------------------
 * OpenGL Rendering
 * --------------------------------------------------------------------------- */
int zstr_gl_comp_render(zstr_gl_comp_t *c)
{
    if (!c) return AVERROR(EINVAL);

    pthread_mutex_lock(&c->lock);

    /* Software composition pass for backup and capture */
    software_composite(c);

    if (!c->null_mode && c->gl_context && c->x_display) {
        glXMakeCurrent(c->x_display, c->x_window, c->gl_context);

        glViewport(0, 0, c->canvas_width, c->canvas_height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, c->canvas_width, c->canvas_height, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        float bg_r = ((c->bg_color >> 24) & 0xFF) / 255.0f;
        float bg_g = ((c->bg_color >> 16) & 0xFF) / 255.0f;
        float bg_b = ((c->bg_color >> 8) & 0xFF) / 255.0f;
        glClearColor(bg_r, bg_g, bg_b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Sort layers by z_order */
        int order[ZSTR_GL_COMP_MAX_LAYERS];
        for (int i = 0; i < c->max_layers; i++) order[i] = i;
        for (int i = 0; i < c->max_layers - 1; i++) {
            for (int j = i + 1; j < c->max_layers; j++) {
                if (c->layers[order[j]].cfg.z_order < c->layers[order[i]].cfg.z_order) {
                    int tmp = order[i];
                    order[i] = order[j];
                    order[j] = tmp;
                }
            }
        }

        for (int idx = 0; idx < c->max_layers; idx++) {
            int l_idx = order[idx];
            comp_layer_t *layer = &c->layers[l_idx];
            if (!layer->cfg.visible || !layer->latest_frame || layer->cfg.alpha <= 0.0f)
                continue;

            AVFrame *f = layer->latest_frame;
            int dst_w = layer->cfg.width > 0 ? layer->cfg.width : f->width;
            int dst_h = layer->cfg.height > 0 ? layer->cfg.height : f->height;
            int dst_x = layer->cfg.x;
            int dst_y = layer->cfg.y;

            /* Draw border if enabled */
            if (layer->cfg.border_width > 0) {
                int bw = layer->cfg.border_width;
                uint32_t bc = layer->cfg.border_color;
                float br = ((bc >> 24) & 0xFF) / 255.0f;
                float bg = ((bc >> 16) & 0xFF) / 255.0f;
                float bb = ((bc >> 8) & 0xFF) / 255.0f;
                float ba = (bc & 0xFF) / 255.0f;

                glUseProgram(0);
                glColor4f(br, bg, bb, ba);
                glRectf(dst_x - bw, dst_y - bw, dst_x + dst_w + bw, dst_y + dst_h + bw);
            }

            /* Draw video frame */
            if (f->format == AV_PIX_FMT_YUV420P && c->prog_yuv420p) {
                if (!layer->tex_y) glGenTextures(1, &layer->tex_y);
                if (!layer->tex_u) glGenTextures(1, &layer->tex_u);
                if (!layer->tex_v) glGenTextures(1, &layer->tex_v);

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, layer->tex_y);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[0]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, f->width, f->height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[0]);

                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, layer->tex_u);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[1]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, f->width / 2, f->height / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[1]);

                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, layer->tex_v);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[2]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, f->width / 2, f->height / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[2]);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

                glUseProgram(c->prog_yuv420p);
                glUniform1i(glGetUniformLocation(c->prog_yuv420p, "y_tex"), 0);
                glUniform1i(glGetUniformLocation(c->prog_yuv420p, "u_tex"), 1);
                glUniform1i(glGetUniformLocation(c->prog_yuv420p, "v_tex"), 2);
                glUniform1f(glGetUniformLocation(c->prog_yuv420p, "alpha"), layer->cfg.alpha);
            } else {
                /* RGB / software converted fallback */
                if (!layer->tex_rgb) glGenTextures(1, &layer->tex_rgb);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, layer->tex_rgb);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                struct SwsContext *sws = sws_getContext(
                    f->width, f->height, f->format,
                    f->width, f->height, AV_PIX_FMT_RGB24,
                    SWS_FAST_BILINEAR, NULL, NULL, NULL
                );
                if (sws) {
                    uint8_t *rgb_buf = malloc(f->width * f->height * 3);
                    uint8_t *dst_data[4] = { rgb_buf, NULL, NULL, NULL };
                    int dst_linesize[4] = { f->width * 3, 0, 0, 0 };
                    sws_scale(sws, (const uint8_t * const*)f->data, f->linesize, 0, f->height, dst_data, dst_linesize);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, f->width, f->height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_buf);
                    free(rgb_buf);
                    sws_freeContext(sws);
                }

                glUseProgram(c->prog_rgb);
                glUniform1i(glGetUniformLocation(c->prog_rgb, "rgb_tex"), 0);
                glUniform1f(glGetUniformLocation(c->prog_rgb, "alpha"), layer->cfg.alpha);
            }

            /* Draw quad */
            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f); glVertex2f((float)dst_x, (float)dst_y);
            glTexCoord2f(1.0f, 0.0f); glVertex2f((float)(dst_x + dst_w), (float)dst_y);
            glTexCoord2f(1.0f, 1.0f); glVertex2f((float)(dst_x + dst_w), (float)(dst_y + dst_h));
            glTexCoord2f(0.0f, 1.0f); glVertex2f((float)dst_x, (float)(dst_y + dst_h));
            glEnd();
            glUseProgram(0);
        }

        glXSwapBuffers(c->x_display, c->x_window);
    }

    c->frames_rendered++;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

int zstr_gl_comp_capture(zstr_gl_comp_t *c, AVFrame *out)
{
    if (!c || !out) return AVERROR(EINVAL);

    pthread_mutex_lock(&c->lock);

    /* Prepare output AVFrame */
    if (out->format == AV_PIX_FMT_NONE) out->format = AV_PIX_FMT_RGBA;
    if (out->width <= 0) out->width = c->canvas_width;
    if (out->height <= 0) out->height = c->canvas_height;

    if (!out->data[0]) {
        int ret = av_frame_get_buffer(out, 64);
        if (ret < 0) {
            pthread_mutex_unlock(&c->lock);
            return ret;
        }
    }

    /* If software canvas is ready, convert it to out->format */
    if (out->format == AV_PIX_FMT_RGBA) {
        const uint8_t *src_data[4] = { c->sw_canvas, NULL, NULL, NULL };
        int src_linesize[4] = { c->canvas_width * 4, 0, 0, 0 };
        av_image_copy(out->data, out->linesize, src_data, src_linesize,
                      AV_PIX_FMT_RGBA, out->width, out->height);
    } else {
        struct SwsContext *sws = sws_getContext(
            c->canvas_width, c->canvas_height, AV_PIX_FMT_RGBA,
            out->width, out->height, out->format,
            SWS_BILINEAR, NULL, NULL, NULL
        );
        if (sws) {
            const uint8_t *src_data[4] = { c->sw_canvas, NULL, NULL, NULL };
            int src_linesize[4] = { c->canvas_width * 4, 0, 0, 0 };
            sws_scale(sws, src_data, src_linesize, 0, c->canvas_height, out->data, out->linesize);
            sws_freeContext(sws);
        }
    }

    pthread_mutex_unlock(&c->lock);
    return 0;
}

void zstr_gl_comp_free(zstr_gl_comp_t **pc)
{
    if (!pc || !*pc) return;
    zstr_gl_comp_t *c = *pc;

    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < c->max_layers; i++) {
        if (c->layers[i].latest_frame) {
            av_frame_free(&c->layers[i].latest_frame);
        }
        if (c->layers[i].tex_y) glDeleteTextures(1, &c->layers[i].tex_y);
        if (c->layers[i].tex_u) glDeleteTextures(1, &c->layers[i].tex_u);
        if (c->layers[i].tex_v) glDeleteTextures(1, &c->layers[i].tex_v);
        if (c->layers[i].tex_rgb) glDeleteTextures(1, &c->layers[i].tex_rgb);
    }

    if (c->prog_yuv420p) glDeleteProgram(c->prog_yuv420p);
    if (c->prog_rgb) glDeleteProgram(c->prog_rgb);

    if (c->gl_context && c->x_display) {
        glXMakeCurrent(c->x_display, None, NULL);
        glXDestroyContext(c->x_display, c->gl_context);
    }
    if (c->x_window && c->x_display) {
        XDestroyWindow(c->x_display, c->x_window);
    }
    if (c->x_display) {
        XCloseDisplay(c->x_display);
    }

    free(c->sw_canvas);
    free(c->window_title);
    free(c->display_name);

    pthread_mutex_unlock(&c->lock);
    pthread_mutex_destroy(&c->lock);
    free(c);
    *pc = NULL;
}

/* ---------------------------------------------------------------------------
 * FFmpeg Outdev Muxer Implementation
 * --------------------------------------------------------------------------- */
typedef struct GLCompMuxContext {
    const AVClass *av_class;
    char *display;
    char *window_title;
    int is_mock;
    zstr_gl_comp_t *comp;
} GLCompMuxContext;

#define OFFSET(x) offsetof(GLCompMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_gl_comp_options[] = {
    { "display",      "X11 display string", OFFSET(display),      AV_OPT_TYPE_STRING, { .str = NULL },           0, 0, ENC },
    { "window_title", "Window title",       OFFSET(window_title), AV_OPT_TYPE_STRING, { .str = "zff gl_comp" }, 0, 0, ENC },
    { "is_mock",      "Force mock mode",    OFFSET(is_mock),      AV_OPT_TYPE_BOOL,   { .i64 = 0 },               0, 1, ENC },
    { NULL }
};

static const AVClass zstr_gl_comp_class = {
    .class_name = "zstr_gl_comp",
    .item_name  = av_default_item_name,
    .option     = zstr_gl_comp_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int gl_comp_write_header(AVFormatContext *s)
{
    GLCompMuxContext *ctx = s->priv_data;
    zstr_gl_comp_config_t cfg = {
        .canvas_width = 1280,
        .canvas_height = 720,
        .window_title = ctx->window_title ? ctx->window_title : "zff gl_comp",
        .display = ctx->display,
        .bg_color = 0x000000FF,
        .is_mock = ctx->is_mock != 0
    };

    ctx->comp = zstr_gl_comp_create(&cfg);
    if (!ctx->comp) return AVERROR(ENOMEM);

    /* Setup default layout based on number of streams */
    unsigned nb_streams = s->nb_streams;
    if (nb_streams == 1) {
        zstr_gl_comp_layer_cfg_t l = { .x = 0, .y = 0, .width = 1280, .height = 720, .z_order = 0, .alpha = 1.0f, .visible = true };
        zstr_gl_comp_configure_layer(ctx->comp, 0, &l);
    } else if (nb_streams == 2) {
        /* Side by side */
        zstr_gl_comp_layer_cfg_t l0 = { .x = 0, .y = 180, .width = 640, .height = 360, .z_order = 0, .alpha = 1.0f, .visible = true };
        zstr_gl_comp_layer_cfg_t l1 = { .x = 640, .y = 180, .width = 640, .height = 360, .z_order = 1, .alpha = 1.0f, .visible = true };
        zstr_gl_comp_configure_layer(ctx->comp, 0, &l0);
        zstr_gl_comp_configure_layer(ctx->comp, 1, &l1);
    } else if (nb_streams >= 3) {
        /* 2x2 grid */
        int gw = 640, gh = 360;
        zstr_gl_comp_layer_cfg_t grid[4] = {
            { .x = 0,  .y = 0,   .width = gw, .height = gh, .z_order = 0, .alpha = 1.0f, .visible = true },
            { .x = gw, .y = 0,   .width = gw, .height = gh, .z_order = 1, .alpha = 1.0f, .visible = true },
            { .x = 0,  .y = gh,  .width = gw, .height = gh, .z_order = 2, .alpha = 1.0f, .visible = true },
            { .x = gw, .y = gh,  .width = gw, .height = gh, .z_order = 3, .alpha = 1.0f, .visible = true }
        };
        for (unsigned i = 0; i < nb_streams && i < 4; i++) {
            zstr_gl_comp_configure_layer(ctx->comp, (int)i, &grid[i]);
        }
    }

    return 0;
}

static int gl_comp_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    GLCompMuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->comp || !pkt) return 0;

    int stream_idx = pkt->stream_index;
    if (stream_idx < 0 || stream_idx >= ctx->comp->max_layers) return 0;

    AVStream *st = s->streams[stream_idx];
    int w = st->codecpar->width > 0 ? st->codecpar->width : 640;
    int h = st->codecpar->height > 0 ? st->codecpar->height : 480;
    enum AVPixelFormat fmt = st->codecpar->format != AV_PIX_FMT_NONE ? st->codecpar->format : AV_PIX_FMT_YUV420P;

    AVFrame *frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    frame->width = w;
    frame->height = h;
    frame->format = fmt;
    frame->pts = pkt->pts;

    int ret = av_frame_get_buffer(frame, 64);
    if (ret >= 0) {
        int copy_sz = av_image_get_buffer_size(fmt, w, h, 1);
        if (pkt->size >= copy_sz) {
            av_image_fill_arrays(frame->data, frame->linesize, pkt->data, fmt, w, h, 1);
        }
        zstr_gl_comp_set_layer_frame(ctx->comp, stream_idx, frame);
        zstr_gl_comp_render(ctx->comp);
    }
    av_frame_free(&frame);
    return 0;
}

static int gl_comp_write_trailer(AVFormatContext *s)
{
    GLCompMuxContext *ctx = s->priv_data;
    if (ctx && ctx->comp) {
        zstr_gl_comp_free(&ctx->comp);
    }
    return 0;
}

const FFOutputFormat ff_zstr_gl_comp_muxer = {
    .p = {
        .name           = "zstr_gl_comp",
        .long_name      = "zff OpenGL Video Compositor Sink",
        .extensions     = NULL,
        .audio_codec    = AV_CODEC_ID_NONE,
        .video_codec    = AV_CODEC_ID_RAWVIDEO,
        .subtitle_codec = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE | AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
        .priv_class     = &zstr_gl_comp_class,
    },
    .priv_data_size = sizeof(GLCompMuxContext),
    .write_header   = gl_comp_write_header,
    .write_packet   = gl_comp_write_packet,
    .write_trailer  = gl_comp_write_trailer,
    .check_bitstream = NULL,
};
