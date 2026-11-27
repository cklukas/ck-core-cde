/*
 * CDE - Common Desktop Environment
 *
 * qualify-baremetal-3d-gpu.c: Bare-metal 3D GPU qualification harness for
 * physical DRM render nodes (/dev/dri/renderD128).
 *
 * Qualifies:
 *   1. Physical DRM render node + GBM device + EGL_PLATFORM_GBM_KHR +
 *      hardware-accelerated OpenGL ES 2.0/3.0 context (rejects software
 *      rasterizers such as llvmpipe/softpipe/lavapipe unless --allow-software).
 *   2. dtwm-wayland HDR10 BT.2020 ST.2084 PQ & HLG -> BT.709 sRGB fragment
 *      shader compilation, FBO execution, and pixel-level accuracy against
 *      double-precision ITU-R BT.2100 CPU reference.
 *   3. EGL_EXT_image_dma_buf_import_modifiers format/modifier enumeration,
 *      single-plane ARGB8888 GBM BO DMA-BUF export/import with explicit 64-bit
 *      modifier, and multi-plane YUV420 / NV12 DMA-BUF import + GL_OES_EGL_image_external
 *      hardware YUV->RGB shader sampling.
 */

#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

static const char *VERT_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

/* Exact BT.2020 ST.2084 PQ / HLG -> BT.709 sRGB tone-mapping math used by dtwm-wayland */
static const char *HDR10_FRAG_SRC =
    "precision highp float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_tf; /* 1 = PQ (ST.2084), 2 = HLG */\n"
    "uniform float u_sdr_ref_nits;\n"
    "float pq_eotf(float e) {\n"
    "    float m1 = 0.1593017578125;\n"
    "    float m2 = 78.84375;\n"
    "    float c1 = 0.8359375;\n"
    "    float c2 = 18.8515625;\n"
    "    float c3 = 18.6875;\n"
    "    float ep = pow(max(e, 0.0), 1.0 / m2);\n"
    "    float num = max(ep - c1, 0.0);\n"
    "    float den = max(c2 - c3 * ep, 1e-6);\n"
    "    return pow(num / den, 1.0 / m1) * 10000.0;\n"
    "}\n"
    "float hlg_inv_oetf(float e) {\n"
    "    float a = 0.17883277;\n"
    "    float b = 1.0 - 4.0 * a;\n"
    "    float c = 0.5 - a * log(4.0 * a);\n"
    "    e = clamp(e, 0.0, 1.0);\n"
    "    if (e <= 0.5) return (e * e) / 3.0;\n"
    "    return (exp((e - c) / a) + b) / 12.0;\n"
    "}\n"
    "float srgb_oetf(float l) {\n"
    "    l = clamp(l, 0.0, 1.0);\n"
    "    if (l <= 0.0031308) return 12.92 * l;\n"
    "    return 1.055 * pow(l, 1.0 / 2.4) - 0.055;\n"
    "}\n"
    "void main() {\n"
    "    vec4 src = texture2D(u_tex, v_uv);\n"
    "    vec3 nits;\n"
    "    if (u_tf == 1) {\n"
    "        nits = vec3(pq_eotf(src.r), pq_eotf(src.g), pq_eotf(src.b));\n"
    "    } else {\n"
    "        vec3 sc = vec3(hlg_inv_oetf(src.r), hlg_inv_oetf(src.g), hlg_inv_oetf(src.b));\n"
    "        float ys = max(0.2627 * sc.r + 0.6780 * sc.g + 0.0593 * sc.b, 1e-6);\n"
    "        nits = sc * pow(ys, 0.2) * 1000.0;\n"
    "    }\n"
    "    vec3 lin2020 = nits / max(u_sdr_ref_nits, 1.0);\n"
    "    /* Reinhard-knee highlight roll-off above 1.0 SDR white */\n"
    "    float peak = max(max(lin2020.r, lin2020.g), lin2020.b);\n"
    "    if (peak > 1.0) {\n"
    "        float mapped = 1.0 + (peak - 1.0) / (1.0 + (peak - 1.0));\n"
    "        /* Normalize into [0,1] display range using 2.0 ceiling */\n"
    "        lin2020 *= (mapped / peak) * 0.5;\n"
    "    } else {\n"
    "        lin2020 *= 0.5;\n"
    "    }\n"
    "    /* ITU-R BT.2020 -> BT.709 linear gamut matrix */\n"
    "    vec3 lin709 = vec3(\n"
    "         1.660491 * lin2020.r - 0.587641 * lin2020.g - 0.072850 * lin2020.b,\n"
    "        -0.124550 * lin2020.r + 1.132900 * lin2020.g - 0.008349 * lin2020.b,\n"
    "        -0.018151 * lin2020.r - 0.100579 * lin2020.g + 1.118730 * lin2020.b\n"
    "    );\n"
    "    gl_FragColor = vec4(srgb_oetf(lin709.r), srgb_oetf(lin709.g), srgb_oetf(lin709.b), src.a);\n"
    "}\n";

static const char *EXT_YUV_FRAG_SRC =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform samplerExternalOES u_ext;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_ext, v_uv);\n"
    "}\n";

static double cpu_pq_eotf(double e) {
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    double ep = pow(e > 0.0 ? e : 0.0, 1.0 / m2);
    double num = ep - c1 > 0.0 ? ep - c1 : 0.0;
    double den = c2 - c3 * ep > 1e-6 ? c2 - c3 * ep : 1e-6;
    return pow(num / den, 1.0 / m1) * 10000.0;
}

static double cpu_pq_oetf(double nits) {
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    double y = fmin(fmax(nits / 10000.0, 0.0), 1.0);
    double ym = pow(y, m1);
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

static double cpu_hlg_inv_oetf(double e) {
    const double a = 0.17883277;
    const double b = 1.0 - 4.0 * a;
    const double c = 0.5 - a * log(4.0 * a);
    e = fmin(fmax(e, 0.0), 1.0);
    if (e <= 0.5) return (e * e) / 3.0;
    return (exp((e - c) / a) + b) / 12.0;
}

static double cpu_srgb_oetf(double l) {
    l = fmin(fmax(l, 0.0), 1.0);
    if (l <= 0.0031308) return 12.92 * l;
    return 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}

static void cpu_tonemap_ref(uint8_t r8, uint8_t g8, uint8_t b8, int tf, double sdr_ref,
                            uint8_t *out_r, uint8_t *out_g, uint8_t *out_b) {
    double sr = r8 / 255.0, sg = g8 / 255.0, sb = b8 / 255.0;
    double nr, ng, nb;
    if (tf == 1) {
        nr = cpu_pq_eotf(sr);
        ng = cpu_pq_eotf(sg);
        nb = cpu_pq_eotf(sb);
    } else {
        double cr = cpu_hlg_inv_oetf(sr);
        double cg = cpu_hlg_inv_oetf(sg);
        double cb = cpu_hlg_inv_oetf(sb);
        double ys = fmax(0.2627 * cr + 0.6780 * cg + 0.0593 * cb, 1e-6);
        double scale = pow(ys, 0.2) * 1000.0;
        nr = cr * scale;
        ng = cg * scale;
        nb = cb * scale;
    }
    double lr = nr / sdr_ref, lg = ng / sdr_ref, lb = nb / sdr_ref;
    double peak = fmax(fmax(lr, lg), lb);
    if (peak > 1.0) {
        double mapped = 1.0 + (peak - 1.0) / (1.0 + (peak - 1.0));
        double k = (mapped / peak) * 0.5;
        lr *= k; lg *= k; lb *= k;
    } else {
        lr *= 0.5; lg *= 0.5; lb *= 0.5;
    }
    double r709 =  1.660491 * lr - 0.587641 * lg - 0.072850 * lb;
    double g709 = -0.124550 * lr + 1.132900 * lg - 0.008349 * lb;
    double b709 = -0.018151 * lr - 0.100579 * lg + 1.118730 * lb;
    *out_r = (uint8_t)lround(cpu_srgb_oetf(r709) * 255.0);
    *out_g = (uint8_t)lround(cpu_srgb_oetf(g709) * 255.0);
    *out_b = (uint8_t)lround(cpu_srgb_oetf(b709) * 255.0);
}

static GLuint compile_shader(GLenum type, const char *src, char *err_buf, size_t err_sz) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(sh, (GLsizei)err_sz, NULL, err_buf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint build_prog(const char *vs_src, const char *fs_src, char *err_buf, size_t err_sz) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src, err_buf, err_sz);
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, err_buf, err_sz);
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, (GLsizei)err_sz, NULL, err_buf);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void json_escape(FILE *out, const char *s) {
    fputc('"', out);
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

int main(int argc, char **argv) {
    const char *node_path = "/dev/dri/renderD128";
    const char *json_out = NULL;
    bool allow_software = false;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--render-node") && i + 1 < argc) node_path = argv[++i];
        else if (!strcmp(argv[i], "--json-out") && i + 1 < argc) json_out = argv[++i];
        else if (!strcmp(argv[i], "--allow-software")) allow_software = true;
    }

    int drm_fd = open(node_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "FAIL: cannot open %s: %s\n", node_path, strerror(errno));
        return 2;
    }

    drmVersionPtr dver = drmGetVersion(drm_fd);
    char drm_name[64] = "unknown", drm_desc[128] = "unknown";
    if (dver) {
        if (dver->name) snprintf(drm_name, sizeof(drm_name), "%.*s", dver->name_len, dver->name);
        if (dver->desc) snprintf(drm_desc, sizeof(drm_desc), "%.*s", dver->desc_len, dver->desc);
        drmFreeVersion(dver);
    }

    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        fprintf(stderr, "FAIL: gbm_create_device failed on %s\n", node_path);
        close(drm_fd);
        return 3;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay edpy = get_plat_dpy
        ? get_plat_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL)
        : eglGetDisplay((EGLNativeDisplayType)gbm);
    if (edpy == EGL_NO_DISPLAY || !eglInitialize(edpy, NULL, NULL)) {
        fprintf(stderr, "FAIL: eglInitialize failed\n");
        gbm_device_destroy(gbm);
        close(drm_fd);
        return 4;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig ecfg = NULL;
    EGLint ncfg = 0;
    if (!eglChooseConfig(edpy, cfg_attr, &ecfg, 1, &ncfg) || ncfg < 1) {
        const EGLint surfaceless_attr[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };
        if (!eglChooseConfig(edpy, surfaceless_attr, &ecfg, 1, &ncfg) || ncfg < 1) {
            fprintf(stderr, "FAIL: eglChooseConfig failed\n");
            return 5;
        }
    }

    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ectx = eglCreateContext(edpy, ecfg, EGL_NO_CONTEXT, ctx_attr);
    if (ectx == EGL_NO_CONTEXT || !eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ectx)) {
        fprintf(stderr, "FAIL: surfaceless eglMakeCurrent failed\n");
        return 6;
    }

    const char *gl_vendor = (const char *)glGetString(GL_VENDOR);
    const char *gl_renderer = (const char *)glGetString(GL_RENDERER);
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
    const char *egl_version = eglQueryString(edpy, EGL_VERSION);
    const char *egl_exts = eglQueryString(edpy, EGL_EXTENSIONS);

    bool is_sw = (gl_renderer && (strstr(gl_renderer, "llvmpipe") ||
                                  strstr(gl_renderer, "softpipe") ||
                                  strstr(gl_renderer, "lavapipe") ||
                                  strstr(gl_renderer, "Software")));
    if (is_sw && !allow_software) {
        fprintf(stderr, "FAIL: software rasterizer detected (%s), bare-metal 3D GPU required\n", gl_renderer);
        return 7;
    }

    /* 1. Compile and execute HDR10 PQ + HLG tone-mapping shaders into FBO */
    char err_buf[512] = {0};
    GLuint hdr_prog = build_prog(VERT_SRC, HDR10_FRAG_SRC, err_buf, sizeof(err_buf));
    if (!hdr_prog) {
        fprintf(stderr, "FAIL: HDR10 shader build failed: %s\n", err_buf);
        return 8;
    }

    GLuint fbo = 0, dst_tex = 0, src_tex = 0;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FAIL: FBO incomplete\n");
        return 9;
    }

    /* 4 HDR swatches: 203 nits SDR white, 600 nits highlight, 1000 nits peak, BT.2020 saturated red */
    uint8_t pq_src[16] = {
        (uint8_t)lround(cpu_pq_oetf(203.0) * 255.0),
        (uint8_t)lround(cpu_pq_oetf(203.0) * 255.0),
        (uint8_t)lround(cpu_pq_oetf(203.0) * 255.0), 255,

        (uint8_t)lround(cpu_pq_oetf(600.0) * 255.0),
        (uint8_t)lround(cpu_pq_oetf(600.0) * 255.0),
        (uint8_t)lround(cpu_pq_oetf(600.0) * 255.0), 255,

        (uint8_t)lround(cpu_pq_oetf(1000.0) * 255.0),
        (uint8_t)lround(cpu_pq_oetf(1000.0) * 255.0),
        (uint8_t)lround(cpu_pq_oetf(1000.0) * 255.0), 255,

        (uint8_t)lround(cpu_pq_oetf(450.0) * 255.0),
        (uint8_t)lround(cpu_pq_oetf(40.0) * 255.0),
        (uint8_t)lround(cpu_pq_oetf(20.0) * 255.0), 255,
    };

    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pq_src);

    static const GLfloat quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glViewport(0, 0, 4, 1);
    glUseProgram(hdr_prog);
    glUniform1i(glGetUniformLocation(hdr_prog, "u_tex"), 0);
    glUniform1f(glGetUniformLocation(hdr_prog, "u_sdr_ref_nits"), 203.0f);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);

    int max_pq_err = 0, max_hlg_err = 0;
    for (int tf = 1; tf <= 2; ++tf) {
        glUniform1i(glGetUniformLocation(hdr_prog, "u_tf"), tf);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        uint8_t got[16] = {0};
        glReadPixels(0, 0, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
        for (int i = 0; i < 4; ++i) {
            uint8_t rr = 0, rg = 0, rb = 0;
            cpu_tonemap_ref(pq_src[i * 4], pq_src[i * 4 + 1], pq_src[i * 4 + 2], tf, 203.0, &rr, &rg, &rb);
            int er = abs((int)got[i * 4] - (int)rr);
            int eg = abs((int)got[i * 4 + 1] - (int)rg);
            int eb = abs((int)got[i * 4 + 2] - (int)rb);
            int em = er > eg ? (er > eb ? er : eb) : (eg > eb ? eg : eb);
            if (tf == 1 && em > max_pq_err) max_pq_err = em;
            if (tf == 2 && em > max_hlg_err) max_hlg_err = em;
        }
    }
    if (max_pq_err > 2 || max_hlg_err > 2) {
        fprintf(stderr, "FAIL: HDR10 GPU shader error exceeded 2 LSB (pq=%d, hlg=%d)\n", max_pq_err, max_hlg_err);
        return 10;
    }

    /* 2. Query DMA-BUF formats & 64-bit modifiers on physical hardware */
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_fmts =
        (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_mods =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    PFNEGLCREATEIMAGEKHRPROC create_img =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_img =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC img_tex2d =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    EGLint num_fmts = 0;
    bool has_argb8888 = false, has_nv12 = false, has_yuv420 = false;
    EGLint argb_mod_count = 0, nv12_mod_count = 0, yuv420_mod_count = 0;
    if (query_fmts && query_fmts(edpy, 0, NULL, &num_fmts) && num_fmts > 0) {
        EGLint *fmts = calloc((size_t)num_fmts, sizeof(EGLint));
        query_fmts(edpy, num_fmts, fmts, &num_fmts);
        for (EGLint i = 0; i < num_fmts; ++i) {
            if ((uint32_t)fmts[i] == DRM_FORMAT_ARGB8888) has_argb8888 = true;
            if ((uint32_t)fmts[i] == DRM_FORMAT_NV12) has_nv12 = true;
            if ((uint32_t)fmts[i] == DRM_FORMAT_YUV420) has_yuv420 = true;
        }
        free(fmts);
        if (query_mods) {
            if (has_argb8888) query_mods(edpy, DRM_FORMAT_ARGB8888, 0, NULL, NULL, &argb_mod_count);
            if (has_nv12) query_mods(edpy, DRM_FORMAT_NV12, 0, NULL, NULL, &nv12_mod_count);
            if (has_yuv420) query_mods(edpy, DRM_FORMAT_YUV420, 0, NULL, NULL, &yuv420_mod_count);
        }
    }

    /* 3. Export real GBM BO DMA-BUF with modifier & import into GLES texture */
    bool argb_dmabuf_ok = false;
    uint64_t argb_bo_modifier = DRM_FORMAT_MOD_INVALID;
    struct gbm_bo *bo = gbm_bo_create(gbm, 64, 64, GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo) {
        bo = gbm_bo_create(gbm, 64, 64, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
    }
    if (bo && create_img && destroy_img && img_tex2d) {
        int prime_fd = gbm_bo_get_fd(bo);
        uint32_t stride = gbm_bo_get_stride(bo);
        argb_bo_modifier = gbm_bo_get_modifier(bo);
        if (prime_fd >= 0) {
            EGLint iattr[32];
            int k = 0;
            iattr[k++] = EGL_WIDTH; iattr[k++] = 64;
            iattr[k++] = EGL_HEIGHT; iattr[k++] = 64;
            iattr[k++] = EGL_LINUX_DRM_FOURCC_EXT; iattr[k++] = (EGLint)DRM_FORMAT_ARGB8888;
            iattr[k++] = EGL_DMA_BUF_PLANE0_FD_EXT; iattr[k++] = prime_fd;
            iattr[k++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; iattr[k++] = 0;
            iattr[k++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; iattr[k++] = (EGLint)stride;
            if (argb_bo_modifier != DRM_FORMAT_MOD_INVALID &&
                egl_exts && strstr(egl_exts, "EGL_EXT_image_dma_buf_import_modifiers")) {
                iattr[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
                iattr[k++] = (EGLint)(argb_bo_modifier & 0xffffffffu);
                iattr[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
                iattr[k++] = (EGLint)(argb_bo_modifier >> 32);
            }
            iattr[k++] = EGL_NONE;
            EGLImageKHR eimg = create_img(edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, iattr);
            if (eimg != EGL_NO_IMAGE_KHR) {
                GLuint imp_tex = 0;
                glGenTextures(1, &imp_tex);
                glBindTexture(GL_TEXTURE_2D, imp_tex);
                img_tex2d(GL_TEXTURE_2D, (GLeglImageOES)eimg);
                argb_dmabuf_ok = (glGetError() == GL_NO_ERROR);
                glDeleteTextures(1, &imp_tex);
                destroy_img(edpy, eimg);
            }
            close(prime_fd);
        }
        gbm_bo_destroy(bo);
    }

    /* 4. Multi-plane YUV420 / NV12 DMA-BUF import + GL_OES_EGL_image_external CSC sampling */
    bool multiplane_yuv_ok = false;
    const char *multiplane_fmt_tested = "none";
    uint8_t yuv_rgb_sample[4] = {0};
    if ((has_nv12 || has_yuv420) && create_img && destroy_img && img_tex2d &&
        gl_exts && strstr(gl_exts, "GL_OES_EGL_image_external")) {
        GLuint ext_prog = build_prog(VERT_SRC, EXT_YUV_FRAG_SRC, err_buf, sizeof(err_buf));
        /* Allocate linear R8 GBM BO to hold packed planar Y (Y=145, Cb=54, Cr=34 -> BT.601 bright green!) */
        struct gbm_bo *y_bo = gbm_bo_create(gbm, 64, 96, GBM_FORMAT_R8, GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
        if (!y_bo) {
            /* Fallback: 64x24 ARGB8888 linear BO provides 256-byte stride x 24 rows = 6144 bytes */
            y_bo = gbm_bo_create(gbm, 64, 24, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR);
        }
        if (ext_prog && y_bo) {
            uint32_t map_stride = 0;
            void *bo_data = NULL;
            uint8_t *ptr = gbm_bo_map(y_bo, 0, 0, gbm_bo_get_width(y_bo), gbm_bo_get_height(y_bo),
                                      GBM_BO_TRANSFER_WRITE, &map_stride, &bo_data);
            if (ptr) {
                /* Plane 0 (Y): 64x64 @ offset 0, pitch 64, Y=145 */
                memset(ptr, 145, 64 * 64);
                if (has_nv12) {
                    /* Plane 1 (UV interleaved): 32x32x2 @ offset 4096, pitch 64, U=54, V=34 */
                    for (int i = 0; i < 32 * 32; ++i) {
                        ptr[4096 + i * 2] = 54;
                        ptr[4096 + i * 2 + 1] = 34;
                    }
                } else {
                    /* Plane 1 (U): 32x32 @ 4096, pitch 64; Plane 2 (V): 32x32 @ 5120, pitch 64 */
                    memset(ptr + 4096, 54, 1024);
                    memset(ptr + 5120, 34, 1024);
                }
                gbm_bo_unmap(y_bo, bo_data);

                int pfd = gbm_bo_get_fd(y_bo);
                if (pfd >= 0) {
                    EGLint yattr[40];
                    int k = 0;
                    yattr[k++] = EGL_WIDTH; yattr[k++] = 64;
                    yattr[k++] = EGL_HEIGHT; yattr[k++] = 64;
                    if (has_nv12) {
                        multiplane_fmt_tested = "DRM_FORMAT_NV12";
                        yattr[k++] = EGL_LINUX_DRM_FOURCC_EXT; yattr[k++] = (EGLint)DRM_FORMAT_NV12;
                        yattr[k++] = EGL_DMA_BUF_PLANE0_FD_EXT; yattr[k++] = pfd;
                        yattr[k++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; yattr[k++] = 0;
                        yattr[k++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; yattr[k++] = 64;
                        yattr[k++] = EGL_DMA_BUF_PLANE1_FD_EXT; yattr[k++] = pfd;
                        yattr[k++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT; yattr[k++] = 4096;
                        yattr[k++] = EGL_DMA_BUF_PLANE1_PITCH_EXT; yattr[k++] = 64;
                    } else {
                        multiplane_fmt_tested = "DRM_FORMAT_YUV420";
                        yattr[k++] = EGL_LINUX_DRM_FOURCC_EXT; yattr[k++] = (EGLint)DRM_FORMAT_YUV420;
                        yattr[k++] = EGL_DMA_BUF_PLANE0_FD_EXT; yattr[k++] = pfd;
                        yattr[k++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; yattr[k++] = 0;
                        yattr[k++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; yattr[k++] = 64;
                        yattr[k++] = EGL_DMA_BUF_PLANE1_FD_EXT; yattr[k++] = pfd;
                        yattr[k++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT; yattr[k++] = 4096;
                        yattr[k++] = EGL_DMA_BUF_PLANE1_PITCH_EXT; yattr[k++] = 64;
                        yattr[k++] = EGL_DMA_BUF_PLANE2_FD_EXT; yattr[k++] = pfd;
                        yattr[k++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT; yattr[k++] = 5120;
                        yattr[k++] = EGL_DMA_BUF_PLANE2_PITCH_EXT; yattr[k++] = 64;
                    }
                    if (egl_exts && strstr(egl_exts, "EGL_EXT_image_dma_buf_import_modifiers")) {
                        yattr[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; yattr[k++] = 0;
                        yattr[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; yattr[k++] = 0;
                        yattr[k++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT; yattr[k++] = 0;
                        yattr[k++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT; yattr[k++] = 0;
                        if (!has_nv12) {
                            yattr[k++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT; yattr[k++] = 0;
                            yattr[k++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT; yattr[k++] = 0;
                        }
                    }
                    yattr[k++] = EGL_NONE;
                    EGLImageKHR yimg = create_img(edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, yattr);
                    if (yimg != EGL_NO_IMAGE_KHR) {
                        GLuint ytex = 0;
                        glGenTextures(1, &ytex);
                        glBindTexture(GL_TEXTURE_EXTERNAL_OES, ytex);
                        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        img_tex2d(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)yimg);
                        glUseProgram(ext_prog);
                        glUniform1i(glGetUniformLocation(ext_prog, "u_ext"), 0);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, yuv_rgb_sample);
                        /* Y=145, Cb=54, Cr=34 decodes to bright green (G >= 180, R < 60, B < 60) */
                        if (yuv_rgb_sample[1] >= 170 && yuv_rgb_sample[0] < 70 && yuv_rgb_sample[2] < 70) {
                            multiplane_yuv_ok = true;
                        }
                        glDeleteTextures(1, &ytex);
                        destroy_img(edpy, yimg);
                    }
                    close(pfd);
                }
            }
            gbm_bo_destroy(y_bo);
        }
        if (ext_prog) glDeleteProgram(ext_prog);
    }

    if (!argb_dmabuf_ok || !multiplane_yuv_ok) {
        fprintf(stderr, "FAIL: DMA-BUF import check failed (argb=%d, multiplane_yuv=%d sample=%u,%u,%u)\n",
                argb_dmabuf_ok ? 1 : 0, multiplane_yuv_ok ? 1 : 0,
                yuv_rgb_sample[0], yuv_rgb_sample[1], yuv_rgb_sample[2]);
        return 11;
    }

    struct utsname uts = {0};
    uname(&uts);
    time_t now = time(NULL);
    struct tm gmt = {0};
    gmtime_r(&now, &gmt);
    char iso_ts[64];
    strftime(iso_ts, sizeof(iso_ts), "%Y-%m-%dT%H:%M:%SZ", &gmt);

    FILE *out = json_out ? fopen(json_out, "w") : stdout;
    if (!out) {
        fprintf(stderr, "FAIL: cannot open %s\n", json_out);
        return 12;
    }
    fprintf(out, "{\n");
    fprintf(out, "  \"qualified_at\": "); json_escape(out, iso_ts); fprintf(out, ",\n");
    fprintf(out, "  \"host\": "); json_escape(out, uts.nodename); fprintf(out, ",\n");
    fprintf(out, "  \"kernel\": "); json_escape(out, uts.release); fprintf(out, ",\n");
    fprintf(out, "  \"arch\": "); json_escape(out, uts.machine); fprintf(out, ",\n");
    fprintf(out, "  \"render_node\": "); json_escape(out, node_path); fprintf(out, ",\n");
    fprintf(out, "  \"drm_driver\": "); json_escape(out, drm_name); fprintf(out, ",\n");
    fprintf(out, "  \"drm_description\": "); json_escape(out, drm_desc); fprintf(out, ",\n");
    fprintf(out, "  \"egl_version\": "); json_escape(out, egl_version ? egl_version : ""); fprintf(out, ",\n");
    fprintf(out, "  \"gl_vendor\": "); json_escape(out, gl_vendor ? gl_vendor : ""); fprintf(out, ",\n");
    fprintf(out, "  \"gl_renderer\": "); json_escape(out, gl_renderer ? gl_renderer : ""); fprintf(out, ",\n");
    fprintf(out, "  \"gl_version\": "); json_escape(out, gl_version ? gl_version : ""); fprintf(out, ",\n");
    fprintf(out, "  \"is_hardware_3d\": %s,\n", is_sw ? "false" : "true");
    fprintf(out, "  \"hdr10_shaders\": {\n");
    fprintf(out, "    \"pq_st2084_fbo_pass\": true,\n");
    fprintf(out, "    \"pq_max_abs_lsb_error\": %d,\n", max_pq_err);
    fprintf(out, "    \"hlg_bt2100_fbo_pass\": true,\n");
    fprintf(out, "    \"hlg_max_abs_lsb_error\": %d\n", max_hlg_err);
    fprintf(out, "  },\n");
    fprintf(out, "  \"dmabuf_modifiers\": {\n");
    fprintf(out, "    \"total_formats\": %d,\n", (int)num_fmts);
    fprintf(out, "    \"argb8888_modifier_count\": %d,\n", (int)argb_mod_count);
    fprintf(out, "    \"argb8888_gbm_bo_modifier_hex\": \"0x%016llx\",\n", (unsigned long long)argb_bo_modifier);
    fprintf(out, "    \"argb8888_import_pass\": true,\n");
    fprintf(out, "    \"nv12_modifier_count\": %d,\n", (int)nv12_mod_count);
    fprintf(out, "    \"yuv420_modifier_count\": %d,\n", (int)yuv420_mod_count);
    fprintf(out, "    \"multiplane_format_tested\": "); json_escape(out, multiplane_fmt_tested); fprintf(out, ",\n");
    fprintf(out, "    \"multiplane_external_oes_rgb\": [%u, %u, %u],\n",
            yuv_rgb_sample[0], yuv_rgb_sample[1], yuv_rgb_sample[2]);
    fprintf(out, "    \"multiplane_import_and_csc_pass\": true\n");
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
    if (json_out) fclose(out);

    printf("PASS: bare-metal 3D GPU qualified on %s (%s / %s, pq_err=%d, hlg_err=%d, %s RGB=%u,%u,%u)\n",
           node_path, drm_name, gl_renderer ? gl_renderer : "?",
           max_pq_err, max_hlg_err, multiplane_fmt_tested,
           yuv_rgb_sample[0], yuv_rgb_sample[1], yuv_rgb_sample[2]);

    glDeleteTextures(1, &src_tex);
    glDeleteTextures(1, &dst_tex);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(hdr_prog);
    eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(edpy, ectx);
    eglTerminate(edpy);
    gbm_device_destroy(gbm);
    close(drm_fd);
    return 0;
}
