// OpenGL ES 2 para el invitado. Las funciones se cargan en tiempo de ejecución, así que sirve
// un contexto GLES 2 (Linux/EGL) o uno de OpenGL de escritorio 2.1+ (Windows), adaptando los shaders.
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

#include "log.h"
#include "thunks.h"

#ifndef _WIN32
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#if defined(_WIN32)
#define GLAPIENTRY __stdcall
#else
#define GLAPIENTRY
#endif

namespace host {

using GLenum = uint32_t;
using GLuint = uint32_t;
using GLint = int32_t;
using GLsizei = int32_t;
using GLfloat = float;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLchar = char;
using GLintptr = intptr_t;
using GLsizeiptr = intptr_t;

constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894, GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895;
constexpr GLenum GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02;
constexpr GLenum GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401;

// Lista de funciones: X(retorno, nombre, (parámetros))
#define GL_FUNCS(X)                                                                                                \
    X(void, glActiveTexture, (GLenum))                                                                             \
    X(void, glAttachShader, (GLuint, GLuint))                                                                      \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))                                                 \
    X(void, glBindBuffer, (GLenum, GLuint))                                                                        \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                                                   \
    X(void, glBindRenderbuffer, (GLenum, GLuint))                                                                  \
    X(void, glBindTexture, (GLenum, GLuint))                                                                       \
    X(void, glBlendEquation, (GLenum))                                                                             \
    X(void, glBlendFunc, (GLenum, GLenum))                                                                         \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                 \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                              \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                                         \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                                                  \
    X(void, glClear, (GLbitfield))                                                                                 \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                    \
    X(void, glClearStencil, (GLint))                                                                               \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                             \
    X(void, glCompileShader, (GLuint))                                                                             \
    X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*))        \
    X(GLuint, glCreateProgram, ())                                                                                 \
    X(GLuint, glCreateShader, (GLenum))                                                                            \
    X(void, glDeleteBuffers, (GLsizei, const GLuint*))                                                             \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*))                                                        \
    X(void, glDeleteProgram, (GLuint))                                                                             \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*))                                                       \
    X(void, glDeleteShader, (GLuint))                                                                              \
    X(void, glDeleteTextures, (GLsizei, const GLuint*))                                                            \
    X(void, glDepthFunc, (GLenum))                                                                                 \
    X(void, glDepthMask, (GLboolean))                                                                              \
    X(void, glDisable, (GLenum))                                                                                   \
    X(void, glDisableVertexAttribArray, (GLuint))                                                                  \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                                \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*))                                               \
    X(void, glEnable, (GLenum))                                                                                    \
    X(void, glEnableVertexAttribArray, (GLuint))                                                                   \
    X(void, glFinish, ())                                                                                          \
    X(void, glFlush, ())                                                                                           \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                           \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                       \
    X(void, glGenBuffers, (GLsizei, GLuint*))                                                                      \
    X(void, glGenFramebuffers, (GLsizei, GLuint*))                                                                 \
    X(void, glGenRenderbuffers, (GLsizei, GLuint*))                                                                \
    X(void, glGenTextures, (GLsizei, GLuint*))                                                                     \
    X(void, glGenerateMipmap, (GLenum))                                                                            \
    X(GLint, glGetAttribLocation, (GLuint, const GLchar*))                                                         \
    X(void, glGetBooleanv, (GLenum, GLboolean*))                                                                   \
    X(GLenum, glGetError, ())                                                                                      \
    X(void, glGetFloatv, (GLenum, GLfloat*))                                                                       \
    X(void, glGetIntegerv, (GLenum, GLint*))                                                                       \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                             \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*))                                                              \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                              \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*))                                                               \
    X(const uint8_t*, glGetString, (GLenum))                                                                       \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*))                                                        \
    X(void, glLineWidth, (GLfloat))                                                                                \
    X(void, glLinkProgram, (GLuint))                                                                               \
    X(void, glPixelStorei, (GLenum, GLint))                                                                        \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                                 \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                             \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                                           \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                                 \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))           \
    X(void, glTexParameterf, (GLenum, GLenum, GLfloat))                                                            \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                                              \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))        \
    X(void, glUniform1f, (GLint, GLfloat))                                                                         \
    X(void, glUniform1fv, (GLint, GLsizei, const GLfloat*))                                                        \
    X(void, glUniform1i, (GLint, GLint))                                                                           \
    X(void, glUniform1iv, (GLint, GLsizei, const GLint*))                                                          \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                                                \
    X(void, glUniform2fv, (GLint, GLsizei, const GLfloat*))                                                        \
    X(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))                                                       \
    X(void, glUniform3fv, (GLint, GLsizei, const GLfloat*))                                                        \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                              \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat*))                                                        \
    X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat*))                                       \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))                                       \
    X(void, glUseProgram, (GLuint))                                                                                \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                       \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))

namespace gl {
#define DECL(ret, name, params) ret(GLAPIENTRY* name) params = nullptr;
GL_FUNCS(DECL)
#undef DECL
void(GLAPIENTRY* glClearDepthf)(GLfloat) = nullptr;
void(GLAPIENTRY* glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
void(GLAPIENTRY* glClearDepth)(double) = nullptr;
}  // namespace gl

namespace {

bool g_desktop = false;  // contexto de OpenGL de escritorio (no ES)

// Ventana redimensionable: el juego dibuja en un framebuffer propio de tamaño fijo, que el juego
// ve como "el framebuffer 0", y al final de cada frame se copia escalado a la ventana.
GLuint g_fbo = 0;
int g_fbo_w = 0, g_fbo_h = 0;
float g_rs = 1.0f;
bool g_main_fb = true;
static inline int rs(GLint v) { return g_main_fb && g_rs != 1.0f ? int(v * g_rs + 0.5f) : v; }
constexpr GLenum GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9,
                 GL_FRAMEBUFFER_BINDING = 0x8CA6, GL_RENDERBUFFER = 0x8D41, GL_TEXTURE_2D = 0x0DE1,
                 GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893,
                 GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_STENCIL_ATTACHMENT = 0x8D20,
                 GL_DEPTH24_STENCIL8 = 0x88F0, GL_SCISSOR_TEST = 0x0C11, GL_COLOR_BUFFER_BIT = 0x4000,
                 GL_LINEAR = 0x2601, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                 GL_TEXTURE_BINDING_2D = 0x8069, GL_RENDERBUFFER_BINDING = 0x8CA7;
std::map<GLenum, uint32_t> g_strings;

// Estado de OpenGL que el juego cambia a cada momento. El juego (cocos2d) repite muchas llamadas
// con los mismos valores; si el estado ya es ese, no se le pasa al driver. GD_NO_GLCACHE=1 lo desactiva.
struct GlCache {
    bool on = true;
    GLuint array_buf = 0, element_buf = 0, program = 0xffffffff;
    GLenum active_unit = 0x84C0;  // GL_TEXTURE0
    GLuint tex2d[16] = {};
    GLenum blend_src = 0xffffffff, blend_dst = 0xffffffff;
    uint32_t attrib_on = 0, attrib_known = 0;
    struct Attr { GLint size = -1; GLenum type = 0; GLboolean norm = 0; GLsizei stride = 0; uint32_t ptr = 0; GLuint buf = 0; };
    Attr attr[16];
} g_glc;

const void* ptr_or_offset(uint32_t v, GLenum binding) {
    GLint bound = 0;
    if (g_glc.on) bound = GLint(binding == GL_ARRAY_BUFFER_BINDING ? g_glc.array_buf : g_glc.element_buf);
    else gl::glGetIntegerv(binding, &bound);
    if (bound || v < mem::kNullGuard) return reinterpret_cast<const void*>(uintptr_t(v));
    return mem::ptr(v);
}

// GLSL ES 1.00 -> GLSL 1.20 de escritorio.
std::string desktop_shader(const std::string& src) {
    static const std::regex precision(R"(precision\s+(lowp|mediump|highp)\s+\w+\s*;)");
    static const std::regex version(R"(#version\s+100[^\n]*)");
    std::string s = std::regex_replace(std::regex_replace(src, precision, ""), version, "");
    return "#version 120\n#define lowp\n#define mediump\n#define highp\n" + s;
}

#ifndef _WIN32
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLSurface g_surf = EGL_NO_SURFACE;
#endif

}  // namespace

bool gl_load(void* (*getproc)(const char*), bool desktop) {
    g_desktop = desktop;
    bool ok = true;
#define LOAD(ret, name, params)                                                       \
    gl::name = reinterpret_cast<decltype(gl::name)>(getproc(#name));                  \
    if (!gl::name) { logf("[gl] falta la función %s", #name); ok = false; }
    GL_FUNCS(LOAD)
#undef LOAD
    gl::glClearDepthf = reinterpret_cast<decltype(gl::glClearDepthf)>(getproc("glClearDepthf"));
    gl::glClearDepth = reinterpret_cast<decltype(gl::glClearDepth)>(getproc("glClearDepth"));
    gl::glBlitFramebuffer = reinterpret_cast<decltype(gl::glBlitFramebuffer)>(getproc("glBlitFramebuffer"));
    if (ok) logf("[gl] %s / %s / %s%s", gl::glGetString(GL_VENDOR), gl::glGetString(GL_RENDERER), gl::glGetString(GL_VERSION),
                 desktop ? " (escritorio: shaders adaptados)" : "");
    return ok;
}

bool gl_offscreen_begin(int w, int h, float scale) {
    if (!gl::glBlitFramebuffer) {
        logf("[gl] sin glBlitFramebuffer: la ventana no se podrá redimensionar");
        return false;
    }
    if (scale > 0.25f && scale < 1.0f) {
        g_rs = scale;
        w = int(w * scale + 0.5f);
        h = int(h * scale + 0.5f);
    }
    GLuint tex = 0, rb = 0;
    gl::glGenTextures(1, &tex);
    gl::glBindTexture(GL_TEXTURE_2D, tex);
    gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::glBindTexture(GL_TEXTURE_2D, 0);
    gl::glGenRenderbuffers(1, &rb);
    gl::glBindRenderbuffer(GL_RENDERBUFFER, rb);
    gl::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    gl::glBindRenderbuffer(GL_RENDERBUFFER, 0);
    gl::glGenFramebuffers(1, &g_fbo);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    gl::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    gl::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    gl::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    if (gl::glCheckFramebufferStatus(GL_FRAMEBUFFER) != 0x8CD5) {
        logf("[gl] framebuffer intermedio incompleto: ventana de tamaño fijo");
        gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
        g_fbo = 0;
        g_rs = 1.0f;
        return false;
    }
    g_fbo_w = w;
    g_fbo_h = h;
    return true;
}

// Copia el frame del juego a la ventana manteniendo la proporción. Devuelve el rectángulo usado.
void gl_present(int win_w, int win_h, int& x, int& y, int& w, int& h) {
    double s = std::min(double(win_w) / g_fbo_w, double(win_h) / g_fbo_h);
    w = int(g_fbo_w * s + 0.5);
    h = int(g_fbo_h * s + 0.5);
    x = (win_w - w) / 2;
    y = (win_h - h) / 2;
    if (!g_fbo) return;
    GLboolean scissor = 0;
    gl::glGetBooleanv(GL_SCISSOR_TEST, &scissor);
    if (scissor) gl::glDisable(GL_SCISSOR_TEST);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, win_w, win_h);
    gl::glClearColor(0, 0, 0, 1);
    gl::glClear(GL_COLOR_BUFFER_BIT);
    gl::glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    gl::glBlitFramebuffer(0, 0, g_fbo_w, g_fbo_h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    gl::glViewport(0, 0, g_fbo_w, g_fbo_h);
    if (scissor) gl::glEnable(GL_SCISSOR_TEST);
}

void gl_fb_size(int& w, int& h) {
    if (g_fbo) w = g_fbo_w, h = g_fbo_h;
}

void gl_read_pixels(int w, int h, void* rgba) { gl::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba); }

#ifndef _WIN32
bool gl_init_offscreen(int w, int h) {
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (getPlatformDisplay) g_dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (g_dpy == EGL_NO_DISPLAY) g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(g_dpy, &major, &minor)) { logf("[gl] eglInitialize falló"); return false; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                               EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &n) || n == 0) { logf("[gl] sin configuración EGL"); return false; }
    const EGLint pb[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    g_surf = eglCreatePbufferSurface(g_dpy, cfg, pb);
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, g_surf, g_surf, ctx)) {
        logf("[gl] no se pudo crear el contexto (err %x)", eglGetError());
        return false;
    }
    return gl_load([](const char* n) { return reinterpret_cast<void*>(eglGetProcAddress(n)); }, false);
}
void gl_swap() { eglSwapBuffers(g_dpy, g_surf); }
#else
bool gl_init_offscreen(int, int) { return false; }
void gl_swap() {}
#endif

void register_gl() {
    using namespace gl;
    // --- sin punteros ---
    g_glc.on = !std::getenv("GD_NO_GLCACHE");
    DEF("glActiveTexture") {
        if (g_glc.on && arg(c, 0) == g_glc.active_unit) return;
        g_glc.active_unit = arg(c, 0);
        gl::glActiveTexture(arg(c, 0));
    });
    DEF("glAttachShader") { gl::glAttachShader(arg(c, 0), arg(c, 1)); });
    DEF("glBindBuffer") {
        GLuint* cur = arg(c, 0) == GL_ARRAY_BUFFER ? &g_glc.array_buf : arg(c, 0) == GL_ELEMENT_ARRAY_BUFFER ? &g_glc.element_buf : nullptr;
        if (g_glc.on && cur && *cur == arg(c, 1)) return;
        if (cur) *cur = arg(c, 1);
        gl::glBindBuffer(arg(c, 0), arg(c, 1));
    });
    DEF("glBindFramebuffer") {
        g_main_fb = arg(c, 1) == 0;
        gl::glBindFramebuffer(arg(c, 0), arg(c, 1) ? arg(c, 1) : g_fbo);
    });
    DEF("glBindRenderbuffer") { gl::glBindRenderbuffer(arg(c, 0), arg(c, 1)); });
    DEF("glBindTexture") {
        uint32_t unit = g_glc.active_unit - 0x84C0;
        bool track = arg(c, 0) == GL_TEXTURE_2D && unit < 16;
        if (g_glc.on && track && g_glc.tex2d[unit] == arg(c, 1)) return;
        if (track) g_glc.tex2d[unit] = arg(c, 1);
        gl::glBindTexture(arg(c, 0), arg(c, 1));
    });
    DEF("glBlendFunc") {
        if (g_glc.on && g_glc.blend_src == arg(c, 0) && g_glc.blend_dst == arg(c, 1)) return;
        g_glc.blend_src = arg(c, 0);
        g_glc.blend_dst = arg(c, 1);
        gl::glBlendFunc(arg(c, 0), arg(c, 1));
    });
    DEF("glBlendFuncSeparate") {
        g_glc.blend_src = g_glc.blend_dst = 0xffffffff;
        gl::glBlendFuncSeparate(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
    });
    DEF("glBlendEquation") { gl::glBlendEquation(arg(c, 0)); });
    DEF("glCheckFramebufferStatus") { ret(c, gl::glCheckFramebufferStatus(arg(c, 0))); });
    DEF("glClear") { gl::glClear(arg(c, 0)); });
    DEF("glClearColor") { gl::glClearColor(argf(c, 0), argf(c, 1), argf(c, 2), argf(c, 3)); });
    DEF("glClearDepthf") {
        if (gl::glClearDepthf) gl::glClearDepthf(argf(c, 0));
        else if (gl::glClearDepth) gl::glClearDepth(argf(c, 0));
    });
    DEF("glClearStencil") { gl::glClearStencil(int(arg(c, 0))); });
    DEF("glColorMask") { gl::glColorMask(uint8_t(arg(c, 0)), uint8_t(arg(c, 1)), uint8_t(arg(c, 2)), uint8_t(arg(c, 3))); });
    DEF("glCompileShader") { gl::glCompileShader(arg(c, 0)); });
    DEF("glCreateProgram") { ret(c, gl::glCreateProgram()); });
    DEF("glCreateShader") { ret(c, gl::glCreateShader(arg(c, 0))); });
    DEF("glDeleteProgram") {
        g_glc.program = 0xffffffff;
        gl::glDeleteProgram(arg(c, 0));
    });
    DEF("glDeleteShader") { gl::glDeleteShader(arg(c, 0)); });
    DEF("glDepthFunc") { gl::glDepthFunc(arg(c, 0)); });
    DEF("glDepthMask") { gl::glDepthMask(uint8_t(arg(c, 0))); });
    DEF("glDisable") { gl::glDisable(arg(c, 0)); });
    DEF("glEnable") { gl::glEnable(arg(c, 0)); });
    DEF("glDisableVertexAttribArray") {
        uint32_t i = arg(c, 0), bit = i < 32 ? 1u << i : 0;
        if (g_glc.on && bit && (g_glc.attrib_known & bit) && !(g_glc.attrib_on & bit)) return;
        g_glc.attrib_known |= bit;
        g_glc.attrib_on &= ~bit;
        gl::glDisableVertexAttribArray(i);
    });
    DEF("glEnableVertexAttribArray") {
        uint32_t i = arg(c, 0), bit = i < 32 ? 1u << i : 0;
        if (g_glc.on && bit && (g_glc.attrib_known & bit) && (g_glc.attrib_on & bit)) return;
        g_glc.attrib_known |= bit;
        g_glc.attrib_on |= bit;
        gl::glEnableVertexAttribArray(i);
    });
    DEF("glDrawArrays") { gl::glDrawArrays(arg(c, 0), int(arg(c, 1)), int(arg(c, 2))); });
    DEF("glFinish") { gl::glFinish(); });
    DEF("glFlush") { gl::glFlush(); });
    DEF("glFramebufferRenderbuffer") { gl::glFramebufferRenderbuffer(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)); });
    DEF("glFramebufferTexture2D") { gl::glFramebufferTexture2D(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), int(arg(c, 4))); });
    DEF("glGenerateMipmap") { gl::glGenerateMipmap(arg(c, 0)); });
    DEF("glGetError") { ret(c, gl::glGetError()); });
    DEF("glLineWidth") { gl::glLineWidth(g_main_fb ? argf(c, 0) * g_rs : argf(c, 0)); });
    DEF("glLinkProgram") { gl::glLinkProgram(arg(c, 0)); });
    DEF("glPixelStorei") { gl::glPixelStorei(arg(c, 0), int(arg(c, 1))); });
    DEF("glRenderbufferStorage") { gl::glRenderbufferStorage(arg(c, 0), arg(c, 1), int(arg(c, 2)), int(arg(c, 3))); });
    DEF("glScissor") { gl::glScissor(rs(arg(c, 0)), rs(arg(c, 1)), rs(arg(c, 2)), rs(arg(c, 3))); });
    DEF("glTexParameteri") { gl::glTexParameteri(arg(c, 0), arg(c, 1), int(arg(c, 2))); });
    DEF("glTexParameterf") { gl::glTexParameterf(arg(c, 0), arg(c, 1), argf(c, 2)); });
    DEF("glUniform1f") { gl::glUniform1f(int(arg(c, 0)), argf(c, 1)); });
    DEF("glUniform1i") { gl::glUniform1i(int(arg(c, 0)), int(arg(c, 1))); });
    DEF("glUniform2f") { gl::glUniform2f(int(arg(c, 0)), argf(c, 1), argf(c, 2)); });
    DEF("glUniform3f") { gl::glUniform3f(int(arg(c, 0)), argf(c, 1), argf(c, 2), argf(c, 3)); });
    DEF("glUniform4f") { gl::glUniform4f(int(arg(c, 0)), argf(c, 1), argf(c, 2), argf(c, 3), argf(c, 4)); });
    DEF("glUseProgram") {
        if (g_glc.on && g_glc.program == arg(c, 0)) return;
        g_glc.program = arg(c, 0);
        gl::glUseProgram(arg(c, 0));
    });
    DEF("glViewport") { gl::glViewport(rs(arg(c, 0)), rs(arg(c, 1)), rs(arg(c, 2)), rs(arg(c, 3))); });

    // --- con punteros ---
    DEF("glBindAttribLocation") { gl::glBindAttribLocation(arg(c, 0), arg(c, 1), args(c, 2)); });
    DEF("glGetUniformLocation") { ret(c, uint32_t(gl::glGetUniformLocation(arg(c, 0), args(c, 1)))); });
    DEF("glGetAttribLocation") { ret(c, uint32_t(gl::glGetAttribLocation(arg(c, 0), args(c, 1)))); });
    DEF("glBufferData") { gl::glBufferData(arg(c, 0), int32_t(arg(c, 1)), argp(c, 2), arg(c, 3)); });
    DEF("glBufferSubData") { gl::glBufferSubData(arg(c, 0), int32_t(arg(c, 1)), int32_t(arg(c, 2)), argp(c, 3)); });
    DEF("glGenBuffers") { gl::glGenBuffers(int(arg(c, 0)), argp<GLuint>(c, 1)); });
    DEF("glGenFramebuffers") { gl::glGenFramebuffers(int(arg(c, 0)), argp<GLuint>(c, 1)); });
    DEF("glGenRenderbuffers") { gl::glGenRenderbuffers(int(arg(c, 0)), argp<GLuint>(c, 1)); });
    DEF("glGenTextures") { gl::glGenTextures(int(arg(c, 0)), argp<GLuint>(c, 1)); });
    DEF("glDeleteBuffers") {
        // Borrar un buffer enlazado lo desenlaza (y deja de valer como origen de los atributos).
        const GLuint* ids = argp<GLuint>(c, 1);
        for (int i = 0; i < int(arg(c, 0)); i++) {
            if (ids[i] == g_glc.array_buf) g_glc.array_buf = 0;
            if (ids[i] == g_glc.element_buf) g_glc.element_buf = 0;
            for (auto& a : g_glc.attr)
                if (a.buf == ids[i]) a.size = -1;
        }
        gl::glDeleteBuffers(int(arg(c, 0)), ids);
    });
    DEF("glDeleteFramebuffers") { gl::glDeleteFramebuffers(int(arg(c, 0)), argp<GLuint>(c, 1)); });
    DEF("glDeleteRenderbuffers") { gl::glDeleteRenderbuffers(int(arg(c, 0)), argp<GLuint>(c, 1)); });
    DEF("glDeleteTextures") {
        const GLuint* ids = argp<GLuint>(c, 1);
        for (int i = 0; i < int(arg(c, 0)); i++)
            for (auto& t : g_glc.tex2d)
                if (t == ids[i]) t = 0;
        gl::glDeleteTextures(int(arg(c, 0)), ids);
    });
    DEF("glGetIntegerv") {
        gl::glGetIntegerv(arg(c, 0), argp<GLint>(c, 1));
        // El juego no debe ver nuestro framebuffer intermedio: para él es el 0.
        if (g_fbo && arg(c, 0) == GL_FRAMEBUFFER_BINDING && GLuint(*argp<GLint>(c, 1)) == g_fbo) *argp<GLint>(c, 1) = 0;
        if (g_main_fb && g_rs != 1.0f && (arg(c, 0) == 0x0BA2 || arg(c, 0) == 0x0C10))
            for (int i = 0; i < 4; i++) argp<GLint>(c, 1)[i] = int(argp<GLint>(c, 1)[i] / g_rs + 0.5f);
    });
    DEF("glGetFloatv") { gl::glGetFloatv(arg(c, 0), argp<GLfloat>(c, 1)); });
    DEF("glGetBooleanv") { gl::glGetBooleanv(arg(c, 0), argp<GLboolean>(c, 1)); });
    DEF("glGetProgramiv") { gl::glGetProgramiv(arg(c, 0), arg(c, 1), argp<GLint>(c, 2)); });
    DEF("glGetShaderiv") { gl::glGetShaderiv(arg(c, 0), arg(c, 1), argp<GLint>(c, 2)); });
    DEF("glGetProgramInfoLog") { gl::glGetProgramInfoLog(arg(c, 0), int(arg(c, 1)), argp<GLsizei>(c, 2), argp<GLchar>(c, 3)); });
    DEF("glGetShaderInfoLog") { gl::glGetShaderInfoLog(arg(c, 0), int(arg(c, 1)), argp<GLsizei>(c, 2), argp<GLchar>(c, 3)); });
    DEF("glGetString") {
        GLenum e = arg(c, 0);
        if (!g_strings.count(e)) {
            const char* s = reinterpret_cast<const char*>(gl::glGetString(e));
            g_strings[e] = mem::strdup(s ? s : "");
        }
        ret(c, g_strings[e]);
    });
    DEF("glShaderSource") {
        int n = int(arg(c, 1));
        uint32_t sp = arg(c, 2), lp = arg(c, 3);
        std::string src;
        for (int i = 0; i < n; i++) {
            const char* p = mem::str(mem::r32(sp + uint32_t(i) * 4));
            int32_t len = lp ? int32_t(mem::r32(lp + uint32_t(i) * 4)) : -1;
            src += len < 0 ? std::string(p) : std::string(p, size_t(len));
        }
        if (g_desktop) src = desktop_shader(src);
        const char* ptr = src.c_str();
        gl::glShaderSource(arg(c, 0), 1, &ptr, nullptr);
    });
    DEF("glTexImage2D") {
        gl::glTexImage2D(arg(c, 0), int(arg(c, 1)), int(arg(c, 2)), int(arg(c, 3)), int(arg(c, 4)), int(arg(c, 5)),
                         arg(c, 6), arg(c, 7), argp(c, 8));
    });
    DEF("glTexSubImage2D") {
        gl::glTexSubImage2D(arg(c, 0), int(arg(c, 1)), int(arg(c, 2)), int(arg(c, 3)), int(arg(c, 4)), int(arg(c, 5)),
                            arg(c, 6), arg(c, 7), argp(c, 8));
    });
    DEF("glCompressedTexImage2D") {
        gl::glCompressedTexImage2D(arg(c, 0), int(arg(c, 1)), arg(c, 2), int(arg(c, 3)), int(arg(c, 4)), int(arg(c, 5)),
                                   int(arg(c, 6)), argp(c, 7));
    });
    DEF("glReadPixels") {
        gl::glReadPixels(int(arg(c, 0)), int(arg(c, 1)), int(arg(c, 2)), int(arg(c, 3)), arg(c, 4), arg(c, 5), argp(c, 6));
    });
    DEF("glUniform1fv") { gl::glUniform1fv(int(arg(c, 0)), int(arg(c, 1)), argp<GLfloat>(c, 2)); });
    DEF("glUniform2fv") { gl::glUniform2fv(int(arg(c, 0)), int(arg(c, 1)), argp<GLfloat>(c, 2)); });
    DEF("glUniform3fv") { gl::glUniform3fv(int(arg(c, 0)), int(arg(c, 1)), argp<GLfloat>(c, 2)); });
    DEF("glUniform4fv") { gl::glUniform4fv(int(arg(c, 0)), int(arg(c, 1)), argp<GLfloat>(c, 2)); });
    DEF("glUniform1iv") { gl::glUniform1iv(int(arg(c, 0)), int(arg(c, 1)), argp<GLint>(c, 2)); });
    DEF("glUniformMatrix3fv") { gl::glUniformMatrix3fv(int(arg(c, 0)), int(arg(c, 1)), uint8_t(arg(c, 2)), argp<GLfloat>(c, 3)); });
    DEF("glUniformMatrix4fv") { gl::glUniformMatrix4fv(int(arg(c, 0)), int(arg(c, 1)), uint8_t(arg(c, 2)), argp<GLfloat>(c, 3)); });
    DEF("glVertexAttribPointer") {
        uint32_t i = arg(c, 0);
        if (i < 16) {
            auto& a = g_glc.attr[i];
            GlCache::Attr n{int(arg(c, 1)), arg(c, 2), GLboolean(arg(c, 3)), int(arg(c, 4)), arg(c, 5), g_glc.array_buf};
            if (g_glc.on && a.size == n.size && a.type == n.type && a.norm == n.norm && a.stride == n.stride && a.ptr == n.ptr && a.buf == n.buf) return;
            a = n;
        }
        gl::glVertexAttribPointer(arg(c, 0), int(arg(c, 1)), arg(c, 2), uint8_t(arg(c, 3)), int(arg(c, 4)),
                                  ptr_or_offset(arg(c, 5), GL_ARRAY_BUFFER_BINDING));
    });
    DEF("glDrawElements") {
        gl::glDrawElements(arg(c, 0), int(arg(c, 1)), arg(c, 2), ptr_or_offset(arg(c, 3), GL_ELEMENT_ARRAY_BUFFER_BINDING));
    });
}

}  // namespace host
