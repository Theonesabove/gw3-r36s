/* opengl_patch.c -- OpenGL ES 2 fixups for Mali-400 / R36S */

#define _GNU_SOURCE

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <EGL/egl.h>

#include "so_util.h"
#include "opengl_patch.h"

extern so_module gw3_mod;

/* Real eglGetProcAddress from libEGL, resolved at patch time. */
static void *(*real_eglGetProcAddress)(const char *) = NULL;

/* ── Pass-through hooks (main.c dynlib table entries) ─────────────────── */

void glBindAttribLocationHook(GLuint prog, GLuint index, const char *name) {
    glBindAttribLocation(prog, index, name);
}

void glVertexAttribPointerHook(GLuint index, GLint size, GLenum type,
                                GLboolean norm, GLsizei stride, const void *ptr) {
    glVertexAttribPointer(index, size, type, norm, stride, ptr);
}

void glEnableHook(GLenum cap)          { glEnable(cap); }
void glDisableHook(GLenum cap)         { glDisable(cap); }
void glDepthMaskHook(GLboolean flag)   { glDepthMask(flag); }

void glBindFramebufferHook(GLenum target, GLuint fbo) {
    glBindFramebuffer(target, fbo);
}

void glFramebufferTexture2DHook(GLenum target, GLenum attachment,
                                 GLenum textarget, GLuint texture, GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO INCOMPLETE: 0x%04x tex=%u\n", status, texture);
        fflush(stderr);
    }
}

void glBlendFuncHook(GLenum sfactor, GLenum dfactor) {
    glBlendFunc(sfactor, dfactor);
}

void glUniform3fvHook(GLint loc, GLsizei count, const GLfloat *v) {
    glUniform3fv(loc, count, v);
}

void glUniform4fvHook(GLint loc, GLsizei count, const GLfloat *v) {
    glUniform4fv(loc, count, v);
}

void glDrawElementsHook(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    glDrawElements(mode, count, type, idx);
}

void glEnableVertexAttribArrayHook(GLuint idx)  { glEnableVertexAttribArray(idx); }
void glDisableVertexAttribArrayHook(GLuint idx) { glDisableVertexAttribArray(idx); }

void glDrawArraysHook(GLenum mode, GLint first, GLsizei count) {
    glDrawArrays(mode, first, count);
}

/* ── glLinkProgramHook: link error reporting ──────────────────────────── */

void glLinkProgramHook(GLuint prog) {
    glLinkProgram(prog);
    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char buf[512] = {0};
        glGetProgramInfoLog(prog, sizeof(buf)-1, NULL, buf);
        fprintf(stderr, "glLinkProgram FAILED prog=%u: %s\n", prog, buf);
        fflush(stderr);
    }
}

void glUseProgramHook(GLuint prog) {
    glUseProgram(prog);
}

void glUniformMatrix4fvHook(GLint location, GLsizei count,
                             GLboolean transpose, const GLfloat *value) {
    glUniformMatrix4fv(location, count, transpose, value);
}

/* ── GL_BGRA_EXT → GL_RGBA fix ────────────────────────────────────────── *
 * Mali-400 does not support GL_BGRA_EXT as an upload format. Swizzle the  *
 * pixel data to GL_RGBA before passing it to the driver.                   */

/* ── Texture quality: anisotropic filtering + trilinear mipmaps ─────────── *
 * After each texture upload we generate mipmaps and apply the best         *
 * filtering the driver supports.  glTexParameteriHook prevents the game    *
 * from later downgrading those filters back to nearest/linear.             */

#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF

static GLfloat g_max_aniso   = 0.0f;
static int     g_aniso_ready = 0;

/* Call once after the GL context is live. */
static void init_aniso(void) {
    if (g_aniso_ready) return;
    g_aniso_ready = 1;
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic"))
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_max_aniso);
}

/* Track which texture IDs have a full mipmap pyramid so we can safely set
 * GL_LINEAR_MIPMAP_LINEAR without blanking textures that have none. */
#define MAX_TEX_ID 16384
static uint8_t g_has_mipmap[MAX_TEX_ID];

static void apply_tex_quality(void) {
    init_aniso();
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    if (id > 0 && id < MAX_TEX_ID) g_has_mipmap[id] = 1;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (g_max_aniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_max_aniso);
}

void glTexParameteriHook(GLenum target, GLenum pname, GLint param) {
    if (target == GL_TEXTURE_2D) {
        if (pname == GL_TEXTURE_MAG_FILTER && param == GL_NEAREST) {
            param = GL_LINEAR;
        } else if (pname == GL_TEXTURE_MIN_FILTER) {
            /* Upgrade nearest/linear to trilinear only if this texture has a
             * mipmap pyramid we generated; otherwise just ensure linear. */
            GLint id = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
            int has = (id > 0 && id < MAX_TEX_ID && g_has_mipmap[id]);
            if (param == GL_NEAREST)
                param = has ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
            else if (param == GL_LINEAR && has)
                param = GL_LINEAR_MIPMAP_LINEAR;
        }
    }
    glTexParameteri(target, pname, param);
}

/* ── GL_BGRA_EXT → GL_RGBA fix ────────────────────────────────────────── */

#define GL_BGRA_EXT 0x80E1

static void *bgra_to_rgba(const void *src, GLsizei width, GLsizei height) {
    size_t n = (size_t)width * (size_t)height * 4;
    uint8_t *buf = malloc(n);
    if (!buf) return NULL;
    const uint8_t *s = src;
    uint8_t *d = buf;
    for (size_t i = 0; i < n; i += 4) {
        d[i+0] = s[i+2];
        d[i+1] = s[i+1];
        d[i+2] = s[i+0];
        d[i+3] = s[i+3];
    }
    return buf;
}

void glTexImage2DHook(GLenum target, GLint level, GLint internalformat,
                      GLsizei width, GLsizei height, GLint border,
                      GLenum format, GLenum type, const void *pixels) {
    if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE && pixels) {
        void *buf = bgra_to_rgba(pixels, width, height);
        if (buf) {
            glTexImage2D(target, level, GL_RGBA, width, height, border,
                         GL_RGBA, type, buf);
            free(buf);
            if (level == 0) { glGenerateMipmap(target); apply_tex_quality(); }
            return;
        }
    }
    glTexImage2D(target, level, internalformat, width, height, border,
                 format, type, pixels);
    if (level == 0) { glGenerateMipmap(target); apply_tex_quality(); }
}

void glTexSubImage2DHook(GLenum target, GLint level,
                          GLint xoffset, GLint yoffset,
                          GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const void *pixels) {
    if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE && pixels) {
        void *buf = bgra_to_rgba(pixels, width, height);
        if (buf) {
            glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                            GL_RGBA, type, buf);
            free(buf);
            return;
        }
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                    format, type, pixels);
}

void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum internalformat,
                                 GLsizei width, GLsizei height, GLint border,
                                 GLsizei imageSize, const void *data) {
    while (glGetError() != GL_NO_ERROR) {}
    glCompressedTexImage2D(target, level, internalformat, width, height,
                           border, imageSize, data);
    GLenum err = glGetError();
    if (err) {
        fprintf(stderr, "CompTexImage2D fmt=0x%04x -> GL error 0x%04x\n",
                internalformat, err);
        fflush(stderr);
    }
    /* Try to generate mipmaps for compressed textures — driver may or may not
     * support this; if it fails we silently skip and only apply anisotropy. */
    if (level == 0) {
        while (glGetError() != GL_NO_ERROR) {}
        glGenerateMipmap(target);
        if (glGetError() == GL_NO_ERROR)
            apply_tex_quality();   /* mipmap succeeded: full trilinear + AF */
        else {
            init_aniso();
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            if (g_max_aniso > 1.0f)
                glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_max_aniso);
        }
    }
}

/* ── Shader hooks ─────────────────────────────────────────────────────── */

void glShaderSourceHook(GLuint shader, GLsizei count,
                         const char **string, const GLint *length) {
    glShaderSource(shader, count, string, length);
}

void glCompileShaderHook(GLuint shader) {
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char buf[1024] = {0};
        glGetShaderInfoLog(shader, sizeof(buf)-1, NULL, buf);
        fprintf(stderr, "glCompileShader FAILED shader=%u: %s\n", shader, buf);
        fflush(stderr);
    }
}

/* ── Softfp ABI thunks ────────────────────────────────────────────────── *
 * libCTW.so (Android armeabi-v7a) passes float args in integer registers  *
 * (soft-float calling convention). libMali.so (system) expects them in    *
 * VFP registers (hard-float). These thunks bridge the gap.                */
#define SOFTFP __attribute__((pcs("aapcs")))

SOFTFP void glUniform1f_abi(GLint l, GLfloat v0)                      { glUniform1f(l, v0); }
SOFTFP void glUniform2f_abi(GLint l, GLfloat v0, GLfloat v1)          { glUniform2f(l, v0, v1); }
SOFTFP void glUniform3f_abi(GLint l, GLfloat v0, GLfloat v1, GLfloat v2) { glUniform3f(l, v0, v1, v2); }
SOFTFP void glTexParameterf_abi(GLenum tgt, GLenum pname, GLfloat p)  { glTexParameterf(tgt, pname, p); }
SOFTFP void glDepthRangef_abi(GLfloat n, GLfloat f)                   { glDepthRangef(n, f); }
SOFTFP void glClearDepthf_abi(GLfloat d)                              { glClearDepthf(d); }
SOFTFP void glLineWidth_abi(GLfloat w)                                { glLineWidth(w); }
SOFTFP void glPolygonOffset_abi(GLfloat factor, GLfloat units)        { glPolygonOffset(factor, units); }
SOFTFP void glSampleCoverage_abi(GLclampf value, GLboolean invert)    { glSampleCoverage(value, invert); }
SOFTFP void glVertexAttrib1f_abi(GLuint idx, GLfloat x)               { glVertexAttrib1f(idx, x); }
SOFTFP void glVertexAttrib2f_abi(GLuint idx, GLfloat x, GLfloat y)    { glVertexAttrib2f(idx, x, y); }
SOFTFP void glVertexAttrib3f_abi(GLuint idx, GLfloat x, GLfloat y, GLfloat z)
    { glVertexAttrib3f(idx, x, y, z); }
SOFTFP void glVertexAttrib4f_abi(GLuint idx, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
    { glVertexAttrib4f(idx, x, y, z, w); }

SOFTFP void glUniform4fHook(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
    { glUniform4f(loc, x, y, z, w); }

SOFTFP void glClearColorHook(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
    { glClearColor(r, g, b, a); }

/* Geometry Wars 3 also calls glBlendColor (4 floats) — bridge it too. */
SOFTFP void glBlendColor_abi(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
    { glBlendColor(r, g, b, a); }

/* ── glMapBufferRange emulation (whole-buffer via GL_OES_mapbuffer) ──── *
 * Mali bifrost-g31 has no GL_EXT_map_buffer_range and OES_mapbuffer is
 * WRITE_ONLY (can't read current VBO contents to shadow). Best available:
 * map the whole bound buffer WRITE_ONLY and hand back base+offset; the engine
 * resolves its own glUnmapBufferOES which flushes it, so partial-flush is a
 * no-op. Not perfectly bounded, but the only variant that renders on this HW. */
#ifndef GL_WRITE_ONLY_OES
#define GL_WRITE_ONLY_OES 0x88B9
#endif
/* Resolve a real GL/EGL entry point. dlsym first: it finds the same GLES
 * library our own gl* calls and the engine's direct GL imports bind to (the
 * one SDL made the context current on). eglGetProcAddress is only a fallback:
 * on glvnd systems (e.g. dArkOS RE, Debian trixie) libEGL.so.1 is the glvnd
 * dispatcher while SDL creates the context through libMali's EGL, so glvnd has
 * no current context and hands back libGLdispatch no-op stubs — glMapBufferOES
 * then returns NULL and the engine writes its vertices to address 0. */
static void *gl_real_proc(const char *name) {
    void *p = name ? dlsym(RTLD_DEFAULT, name) : NULL;
    if (!p && real_eglGetProcAddress)
        p = real_eglGetProcAddress(name);
    return p;
}

static void *(*p_glMapBufferOES)(GLenum, GLenum);
static void *glMapBufferRange_emu(GLenum target, GLintptr offset,
                                  GLsizeiptr length, GLbitfield access) {
    (void)length; (void)access;
    if (!p_glMapBufferOES)
        p_glMapBufferOES = (void *(*)(GLenum, GLenum))
            gl_real_proc("glMapBufferOES");
    if (!p_glMapBufferOES) return NULL;
    void *base = p_glMapBufferOES(target, GL_WRITE_ONLY_OES);
    return base ? (char *)base + offset : NULL;
}
static void glFlushMappedBufferRange_emu(GLenum target, GLintptr offset,
                                         GLsizeiptr length) {
    (void)target; (void)offset; (void)length;
}

/* ── eglGetProcAddress override ───────────────────────────────────────── *
 * The engine imports ONLY eglGetProcAddress from EGL and pulls every GL/EGL
 * entry point through it. For float-taking entry points we hand back a
 * soft-float→hard-float thunk (the engine is armeabi-v7a soft-float; the Mali
 * procs are hard-float). For the missing map-buffer-range extension we hand
 * back the OES_mapbuffer emulation above. Everything else is the real proc. */
static const struct { const char *name; void *fn; } g_gl_overrides[] = {
    /* missing extension → emulation */
    { "glMapBufferRange",             (void *)glMapBufferRange_emu        },
    { "glMapBufferRangeEXT",          (void *)glMapBufferRange_emu        },
    { "glFlushMappedBufferRange",     (void *)glFlushMappedBufferRange_emu },
    { "glFlushMappedBufferRangeEXT",  (void *)glFlushMappedBufferRange_emu },
    /* soft-float ABI thunks (every float-parameter GLES2 entry point) */
    { "glClearColor",     (void *)glClearColorHook   },
    { "glBlendColor",     (void *)glBlendColor_abi    },
    { "glClearDepthf",    (void *)glClearDepthf_abi   },
    { "glDepthRangef",    (void *)glDepthRangef_abi   },
    { "glLineWidth",      (void *)glLineWidth_abi     },
    { "glPolygonOffset",  (void *)glPolygonOffset_abi },
    { "glSampleCoverage", (void *)glSampleCoverage_abi },
    { "glTexParameterf",  (void *)glTexParameterf_abi },
    { "glUniform1f",      (void *)glUniform1f_abi     },
    { "glUniform2f",      (void *)glUniform2f_abi     },
    { "glUniform3f",      (void *)glUniform3f_abi     },
    { "glUniform4f",      (void *)glUniform4fHook     },
    { "glVertexAttrib1f", (void *)glVertexAttrib1f_abi },
    { "glVertexAttrib2f", (void *)glVertexAttrib2f_abi },
    { "glVertexAttrib3f", (void *)glVertexAttrib3f_abi },
    { "glVertexAttrib4f", (void *)glVertexAttrib4f_abi },
};

void *eglGetProcAddress_ovr(const char *name) {
    if (name) {
        for (size_t i = 0; i < sizeof(g_gl_overrides) / sizeof(g_gl_overrides[0]); i++) {
            if (strcmp(name, g_gl_overrides[i].name) == 0) {
                fprintf(stderr, "GW3 GL PROC: %-34s -> %p  (shim)\n",
                        name, g_gl_overrides[i].fn);
                fflush(stderr);
                return g_gl_overrides[i].fn;
            }
        }
    }
    void *p = gl_real_proc(name);
    fprintf(stderr, "GW3 GL PROC: %-34s -> %p%s\n",
            name ? name : "(null)", p, p ? "" : "   *** NULL ***");
    fflush(stderr);
    return p;
}

/* ── Patch entry ──────────────────────────────────────────────────────── */

void patch_opengl(void) {
    real_eglGetProcAddress =
        (void *(*)(const char *))dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    if (!real_eglGetProcAddress)
        fprintf(stderr, "patch_opengl: WARNING real eglGetProcAddress not found\n");

    /* GTA-CTW specific GL hooks — these only fire if GW3 imports the symbols
     * directly (it doesn't; it uses eglGetProcAddress). Kept as harmless no-ops
     * when so_symbol returns 0. */
    hook_addr(so_symbol(&gw3_mod, "glVertexAttribPointer"),
              (uintptr_t)glVertexAttribPointerHook);
    hook_addr(so_symbol(&gw3_mod, "glDrawArrays"),
              (uintptr_t)glDrawArraysHook);
    hook_addr(so_symbol(&gw3_mod, "glLinkProgram"),
              (uintptr_t)glLinkProgramHook);
    hook_addr(so_symbol(&gw3_mod, "glUseProgram"),
              (uintptr_t)glUseProgramHook);
    hook_addr(so_symbol(&gw3_mod, "glTexParameteri"),
              (uintptr_t)glTexParameteriHook);
}
