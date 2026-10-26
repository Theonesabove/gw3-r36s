/* jni_patch.c -- Fake Java Native Interface for GTA CTW on R36S
 *
 * Ported from TheOfficialFloW/gtactw_vita (MIT License)
 * Adapted for Linux/SDL2/R36S gamepad.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <locale.h>
#include <unistd.h>
#include <math.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "so_util.h"
#include "jni_patch.h"
#include "qol.h"

extern so_module gw3_mod;
extern char g_data_path[512];

/* The engine concatenates the files-dir string with a bare file name and no
 * separator (observed: "<dir>" + "savegame.dat" -> "/roms/ports/gw3/gw3savegame.dat"),
 * so the dir we hand to setGameFilesDir/setPrivateFilesDir must END with '/'. */
static const char *data_path_slash(void) {
    static char buf[520];
    if (!buf[0]) {
        size_t n = strlen(g_data_path);
        snprintf(buf, sizeof(buf), "%s%s", g_data_path,
                 (n && g_data_path[n - 1] == '/') ? "" : "/");
    }
    return buf;
}

/* Gamepad state updated each frame by poll_input() */
extern SDL_GameController *g_gamepad;
extern int g_gamepad_buttons;
extern float g_gamepad_axis[6]; /* LX LY RX RY L2 R2 */

/* ── JNI method-id ↔ name tracking (DISCOVERY build) ─────────────────── *
 * We do not yet know GW3's JNI contract, so instead of hard-coding GTA's
 * method names we record every method the engine looks up and every native
 * it registers, and log them. Feed the log back to wire up the real driver. */
static char *g_mid_name[1024];
static char *g_mid_sig[1024];
static int   g_mid_count = 0;

/* GetMethodID / GetStaticMethodID / GetFieldID / GetStaticFieldID all share the
 * (env, clazz, name, sig) shape.  GW3's Java bridge (sub_0x23503c) re-resolves
 * the id on EVERY call rather than caching it, and the name it passes is the
 * char data of a heap std::string, so we intern by value into our own storage
 * and hand back a stable small-integer token. */
static int mid_intern(const char *name, const char *sig, const char *kind) {
    if (!name) return 0;
    for (int i = 1; i <= g_mid_count; i++)
        if (g_mid_name[i] && !strcmp(g_mid_name[i], name)) return i;
    if (g_mid_count >= 1023) return 0;
    int id = ++g_mid_count;
    g_mid_name[id] = strdup(name);
    g_mid_sig[id]  = strdup(sig ? sig : "");
    /* Always log a first sighting - this is the JNI surface we must emulate. */
    fprintf(stderr, "JNI: %s #%d %s %s\n", kind, id, g_mid_name[id], g_mid_sig[id]);
    return id;
}

static const char *mid_lookup(int id) {
    if (id > 0 && id <= g_mid_count) return g_mid_name[id];
    return "?";
}

/* Trace helper: collapses consecutive identical lines and, once the engine is
 * in its steady-state draw loop (it polls a small cycle of JNI getters every
 * frame), mutes entirely after a budget so the log stays usable. */
static void jni_tracef(const char *fmt, ...) {
    static char last[256];
    static unsigned long reps;
    static long budget = 3000;
    char cur[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cur, sizeof(cur), fmt, ap);
    va_end(ap);
    if (strcmp(cur, last) == 0) { reps++; return; }
    if (reps) { fprintf(stderr, "  (previous line x%lu)\n", reps + 1); reps = 0; }
    strcpy(last, cur);
    if (budget < 0) return;
    if (budget-- == 0) { fprintf(stderr, "JNI: trace budget reached — muting per-frame JNI logging\n"); return; }
    fputs(cur, stderr);
}

/* ── fake_vm / fake_env buffers ─────────────────────────────────────────── */
char fake_vm[0x1000];
char fake_env[0x1000];

/* Stub for any JNI vtable slot that hasn't been implemented.
 * Returns 0 and logs which slot was hit (via LR → offset into table). */
static int jni_unimpl(void) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    uintptr_t base = (uintptr_t)fake_env;
    if (lr >= base && lr < base + sizeof(fake_env))
        fprintf(stderr, "JNI: unimplemented env slot at offset 0x%x\n",
                (unsigned)(lr - base - 4));
    else
        fprintf(stderr, "JNI: unimplemented call (LR=0x%08x)\n", (unsigned)lr);
    return 0;
}

static void fill_table_with_stub(void *table, size_t size) {
    uintptr_t *p = (uintptr_t *)table;
    size_t n = size / sizeof(uintptr_t);
    for (size_t i = 0; i < n; i++)
        p[i] = (uintptr_t)jni_unimpl;
}

/* SDL window/context needed for GL operations */
extern SDL_Window   *g_window;
extern SDL_GLContext g_gl_ctx;

/* Real panel dimensions, filled in by main() from the SDL window after it is
 * created FULLSCREEN_DESKTOP.  The engine derives its internal render
 * resolution AND aspect ratio from what we feed setDeviceScreenSize /
 * setAppWindowSize / the GameConfig getters below, so these must be the
 * native panel size, not a hardcoded guess.  Default to config.h until set. */
extern int g_screen_w;
extern int g_screen_h;

/* ── JNI implementations ────────────────────────────────────────────────── */

static int ret0(void) { return 0; }
static int ret1(void) { return 1; }

static int GetDeviceInfo(void) { return 0; }

static int GetDeviceType(void) {
    return (DEVICE_MEMORY_MB << 6) | (3 << 2) | 0x1;
}

static int GetDeviceLocale(void) {
    /* 0=EN 1=FR 2=DE 3=IT 4=ES 5=JP */
    const char *lang = getenv("LANG");
    if (!lang) return 0;
    if (!strncmp(lang, "fr", 2)) return 1;
    if (!strncmp(lang, "de", 2)) return 2;
    if (!strncmp(lang, "it", 2)) return 3;
    if (!strncmp(lang, "es", 2)) return 4;
    if (!strncmp(lang, "ja", 2)) return 5;
    return 0;
}

static int GetGamepadType(int port) {
    if (port != 0) return -1;
    return 8; /* PS3 controller */
}

static int GetGamepadButtons(int port) {
    if (port != 0) return 0;
    return g_gamepad_buttons;
}

static float GetGamepadAxis(int port, int axis) {
    if (port != 0 || axis < 0 || axis > 5) return 0.0f;
    float v = g_gamepad_axis[axis];
    if (axis < 4 && fabsf(v) < STICK_DEADZONE) return 0.0f;
    return v;
}

/* GetGamepadTrack(port, p1, p2) → int — camera/right-stick track delta.
 * Original game used virtual touchpad; we map right analog stick (axes 2/3).
 * Return format: (dx << 16) | (dy & 0xFFFF), each signed int16 in [-128, 127]. */
static int GetGamepadTrack(int port, int p1, int p2) {
    if (port != 0) return 0;
    float rx = g_gamepad_axis[2];
    float ry = g_gamepad_axis[3];
    if (fabsf(rx) < STICK_DEADZONE) rx = 0.0f;
    if (fabsf(ry) < STICK_DEADZONE) ry = 0.0f;
    int dx = (int)(rx * 128.0f);
    int dy = (int)(ry * 128.0f);
    return (dx << 16) | (dy & 0xFFFF);
}

static int GetSupportPauseResume(void) { return 1; }

static int HasAppLocalValue(char *key) {
    return (key && strcmp(key, "STORAGE_ROOT") == 0) ? 1 : 0;
}

static int GetSpecialBuildType(void) { return 0; }

static int GetTotalMemory(void) { return DEVICE_MEMORY_MB; }

static int GetAvailableMemory(void) { return DEVICE_MEMORY_MB / 2; }

static char *GetAndroidBuildinfo(int type) {
    (void)type;
    return "Linux;R36S;1.0";
}

static int InitEGLAndGLES2(void) {
    extern SDL_Window *g_window;
    extern SDL_GLContext g_gl_ctx;
    fprintf(stderr, "InitEGLAndGLES2: SDL_GL_MakeCurrent window=%p ctx=%p\n",
            (void*)g_window, (void*)g_gl_ctx);
    fflush(stderr);
    if (g_window && g_gl_ctx)
        SDL_GL_MakeCurrent(g_window, g_gl_ctx);
    return 1;
}

static int swapBuffers(void) {
    SDL_GL_SwapWindow(g_window);
    return 1;
}

static int makeCurrent(void) {
    extern SDL_Window *g_window;
    extern SDL_GLContext g_gl_ctx;
    if (g_window && g_gl_ctx)
        SDL_GL_MakeCurrent(g_window, g_gl_ctx);
    return 1;
}

static int unMakeCurrent(void) {
    SDL_GL_MakeCurrent(g_window, NULL);
    return 1;
}

static char *getAppLocalValue(char *key) {
    if (strcmp(key, "STORAGE_ROOT") == 0)
        return g_data_path;
    return NULL;
}

static char *FileGetArchiveName(int type) {
    switch (type) {
    case 0: return OBB_RELPATH;  /* this version uses 0-indexed OBBs */
    case 1: return OBB_RELPATH;  /* keep 1-indexed for safety */
    default: return NULL;
    }
}

static int DeleteFile(char *file) {
    char path[640];
    snprintf(path, sizeof(path), "%s/%s", g_data_path, file);
    return remove(path) == 0 ? 1 : 0;
}

/* ── Input forwarding ──────────────────────────────────────────────────────
 * GW3 reads live pad state straight out of g_JoypadStates (dynsym 0x9240a8):
 * 4 joypads x 9 words = { u32 buttonMask, f32 axis[8] }.  We own joypad 0 and
 * rewrite it every frame from SDL.  Button bit order and axis order come from
 * gw3_engine_button_mask() / gw3_engine_axis() in main.c (see the note there).
 */
static void pump_joy_input(void) {
    extern so_module gw3_mod;
    extern int   gw3_engine_button_mask(void);
    extern float gw3_engine_axis(int);
    extern void  gw3_input_frame(void);
    static int  *joy = NULL;
    static int   resolved = 0;
    if (!resolved) {
        joy = (int *)so_symbol(&gw3_mod, "g_JoypadStates");
        resolved = 1;
        fprintf(stderr, "GW3 input: g_JoypadStates @%p\n", (void *)joy);
    }
    gw3_input_frame();                          /* latch this frame's pad state */
    if (!joy) return;
    joy[0] = gw3_engine_button_mask();          /* joy0 word 0 = button mask */
    for (int a = 0; a < 8; a++) {
        float v = gw3_engine_axis(a);
        memcpy(&joy[1 + a], &v, sizeof v);      /* joy0 words 1..8 = axis[0..7] */
    }
}

/* ── JNI vtable callbacks (DISCOVERY build: log everything, return benign) ── */

/* Forward decls (defined later, in the string section) */
static char *NewStringUTF(void *env, char *bytes);
static char *GetStringUTFChars(void *env, char *str, int *isCopy);

/* Config getters the engine reads from the GameConfig object.  Return sensible
 * defaults so viewInitGameConfig proceeds past resolution setup.  The width /
 * height getters MUST report the real panel size: viewInitGameConfig computes
 * the internal render target from min(appWindowWidth, cap) and derives its
 * height from appWindowWidth / (appWindowWidth/appWindowHeight), i.e. straight
 * from this aspect ratio.  A hardcoded 1024x768 on a 720x720 panel is what made
 * the picture stretched, then overspill the screen once gameplay switched to
 * rendering the backbuffer render target directly. */
/* 1.3.0 "Evolved" keeps its controller-first console UI for Android TV
 * (GameConfig.tvDevice).  Set in jni_load() when the engine has the 1.3.0
 * entry point setJoystickConnected; GW3_PHONE=1 forces the phone UI. */
int g_tv_mode = 0;

static int config_int_value(const char *name) {
    if (!name) return 0;
    if (!strcmp(name, "tvDevice"))        return g_tv_mode;
    if (!strcmp(name, "nativeWidth"))     return g_screen_w;
    if (!strcmp(name, "nativeHeight"))    return g_screen_h;
    if (!strcmp(name, "targetWidth"))     return g_screen_w;
    if (!strcmp(name, "targetHeight"))    return g_screen_h;
    return 0;
}

static void *config_obj_value(const char *name) {
    if (name && (strstr(name, "Path") || strstr(name, "path") ||
                 strstr(name, "Dir")  || strstr(name, "Storage") ||
                 strstr(name, "obb")  || strstr(name, "Obb") ||
                 strstr(name, "File") || strstr(name, "Name"))) {
        return NewStringUTF(NULL, (char *)data_path_slash());
    }
    return NewStringUTF(NULL, "");
}

static int CallBooleanMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    jni_tracef("JNI: CallBooleanMethod id=%d (%s)\n", id, mid_lookup(id));
    /* Instance-method booleans, and GetBooleanField (GameConfig.tvDevice). */
    return config_int_value(mid_lookup(id));
}

/* pcs("aapcs"): float return must go in r0 */
__attribute__((pcs("aapcs")))
static float CallFloatMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    jni_tracef("JNI: CallFloatMethod id=%d (%s)\n", id, mid_lookup(id));
    return 1.0f;
}

static int CallIntMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    const char *name = mid_lookup(id);
    int v = config_int_value(name);
    jni_tracef("JNI: CallIntMethod id=%d (%s) -> %d\n", id, name, v);
    return v;
}

static void *CallObjectMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    const char *name = mid_lookup(id);
    void *v = config_obj_value(name);
    jni_tracef("JNI: CallObjectMethod id=%d (%s) -> %p\n", id, name, v);
    return v;
}

static void CallVoidMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)id; (void)args;
    jni_tracef("JNI: CallVoidMethod id=%d (%s)\n", id, mid_lookup(id));
}

static int GetMethodID(void *env, void *cls, const char *name, const char *sig) {
    (void)env; (void)cls; return mid_intern(name, sig, "GetMethodID");
}
static int GetStaticMethodID(void *env, void *cls, const char *name, const char *sig) {
    (void)env; (void)cls; return mid_intern(name, sig, "GetStaticMethodID");
}
static int GetFieldID(void *env, void *cls, const char *name, const char *sig) {
    (void)env; (void)cls; return mid_intern(name, sig, "GetFieldID");
}
static int GetStaticFieldID(void *env, void *cls, const char *name, const char *sig) {
    (void)env; (void)cls; return mid_intern(name, sig, "GetStaticFieldID");
}

/* ── GW3Activity / CommonAPI static-method bridge ───────────────────────────
 * The engine's Java surface (strings at .rodata 0x7bab88..0x7badb8) is:
 *   SignInToAppStore / SignOutOfAppStore / SignedInToAppStore /
 *   ConnectingToAppStore / OpenURL / IsOtherAudioPlaying / CloudSaveRead /
 *   CloudSaveWrite / GiveAchievement / DisplayAchievementsUI /
 *   IsConnectedToInternet / AppShutdown / DisplayLeaderboardsUI / Facebook*.
 * We are offline with no store and no cloud save, so every predicate answers
 * false and every getter answers an empty string: nothing for the front end to
 * wait on.  Anything unrecognised is logged by mid_intern above. */
static int java_static_bool(const char *name) {
    if (!strcmp(name, "hasJoystick") || !strcmp(name, "isControllerConnected") ||
        !strcmp(name, "isTVDevice")) return 1;
    return 0;   /* offline / signed-out / no other audio */
}

/* ── Java byte[] (1.3.0 cloud save) ─────────────────────────────────────────
 * A jbyteArray is a pointer to { magic, len, data[] }. */
#define JARR_MAGIC 0x4A415252u
typedef struct { uint32_t magic, len; uint8_t data[]; } jarr_t;
/* Arguments of Java calls are scanned for arrays, so membership is checked
 * against the arrays we handed out instead of dereferencing arbitrary words. */
static jarr_t *g_jarrs[64];
static jarr_t *jarr(void *o) {
    if (!o) return NULL;
    for (int i = 0; i < 64; i++) if (g_jarrs[i] == o) return (jarr_t *)o;
    return NULL;
}
static void jarr_track(jarr_t *a) {
    static unsigned next;
    g_jarrs[next++ % 64] = a;   /* ponytail: ring of 64, oldest arrays forgotten (leaked, never freed) */
}
static void *NewByteArray(void *env, int len) {
    (void)env;
    if (len < 0) len = 0;
    jarr_t *a = calloc(1, sizeof(jarr_t) + (size_t)len + 1);
    if (!a) return NULL;
    a->magic = JARR_MAGIC; a->len = (uint32_t)len;
    jarr_track(a);
    return a;
}
static int GetArrayLength(void *env, void *o) { (void)env; jarr_t *a = jarr(o); return a ? (int)a->len : 0; }
static void *GetByteArrayElements(void *env, void *o, int *isCopy) {
    (void)env; if (isCopy) *isCopy = 0;
    jarr_t *a = jarr(o); return a ? a->data : NULL;
}
static void ByteArrayRegion(void *o, int start, int len, uint8_t *buf, int set) {
    jarr_t *a = jarr(o);
    if (!a || start < 0 || len < 0 || (uint32_t)start + (uint32_t)len > a->len) return;
    if (set) memcpy(a->data + start, buf, (size_t)len); else memcpy(buf, a->data + start, (size_t)len);
}
static void GetByteArrayRegion(void *env, void *o, int start, int len, uint8_t *buf) { (void)env; ByteArrayRegion(o, start, len, buf, 0); }
static void SetByteArrayRegion(void *env, void *o, int start, int len, uint8_t *buf) { (void)env; ByteArrayRegion(o, start, len, buf, 1); }

/* 1.3.0 keeps its save in "the cloud": CloudSaveRead asks Java to fetch it and
 * answer later through onSaveGameReadComplete(byte[]); CloudSaveWrite hands
 * over a byte[].  Both go to cloudsave.bin next to the game.  The read answer
 * is delivered from the draw loop (as Java would, on its own thread). */
static volatile int g_cloud_read_pending = 0;
static void cloud_path(char *out, size_t n) { snprintf(out, n, "%s/cloudsave.bin", g_data_path); }

static void java_static_effects(const char *name, uintptr_t *args) {
    if (!strcmp(name, "CloudSaveRead")) { g_cloud_read_pending = 1; return; }
    if (!strncmp(name, "CloudSave", 9)) {           /* CloudSaveWrite / ...: find the byte[] */
        for (int k = 0; args && k < 4; k++) {
            jarr_t *a = jarr((void *)args[k]);
            if (!a) continue;
            char p[600], tmp[620]; cloud_path(p, sizeof p);
            snprintf(tmp, sizeof tmp, "%s.tmp", p);
            FILE *f = fopen(tmp, "wb");
            if (f) {
                int ok = fwrite(a->data, 1, a->len, f) == a->len;
                ok &= fclose(f) == 0;
                if (ok) rename(tmp, p);
                fprintf(stderr, "java: %s -> %u bytes%s\n", name, a->len, ok ? "" : " (write failed)");
            }
            return;
        }
        return;
    }
    if (!strcmp(name, "AppShutdown")) {             /* Quit from the game's menu */
        fprintf(stderr, "java: AppShutdown -> exiting\n");
        fflush(stderr);
        _exit(0);
    }
}

void jni_deliver_cloud_read(void) {
    if (!g_cloud_read_pending) return;
    g_cloud_read_pending = 0;
    void (*cb)(void *, void *, void *) = (void *)so_symbol(&gw3_mod,
        "Java_com_activision_gw3_common_GW3JNILib_onSaveGameReadComplete");
    if (!cb) return;
    char p[600]; cloud_path(p, sizeof p);
    void *arr = NULL;
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len > 0 && (arr = NewByteArray(NULL, (int)len)) != NULL &&
            fread(((jarr_t *)arr)->data, 1, (size_t)len, f) != (size_t)len) arr = NULL;
        fclose(f);
    }
    fprintf(stderr, "java: CloudSaveRead -> %s\n", arr ? "cloudsave.bin" : "no save yet");
    static int fake_lib;
    cb(fake_env, &fake_lib, arr);
}

static int CallStaticBooleanMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    const char *name = mid_lookup(id);
    java_static_effects(name, args);
    int v = java_static_bool(name);
    jni_tracef("JNI: CallStaticBooleanMethod %s -> %d\n", name, v);
    return v;
}

static void *CallStaticObjectMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    const char *name = mid_lookup(id);
    java_static_effects(name, args);
    void *v = config_obj_value(name);
    jni_tracef("JNI: CallStaticObjectMethod %s -> \"%s\"\n", name, (char *)v);
    return v;
}

static int CallStaticIntMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    const char *name = mid_lookup(id);
    java_static_effects(name, args);
    int v = config_int_value(name);
    jni_tracef("JNI: CallStaticIntMethod %s -> %d\n", name, v);
    return v;
}

static void CallStaticVoidMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj;
    jni_tracef("JNI: CallStaticVoidMethod %s\n", mid_lookup(id));
    java_static_effects(mid_lookup(id), args);
}

/* The plain (variadic) and ...A (jvalue[]) variants: collect the first
 * argument words and share the ...V handlers (which only scan them). */
#define JNI_VARIANTS(R, X)                                                     \
static R X##Va(void *env, void *obj, int id, ...) {                           \
    uintptr_t w[4]; va_list ap; va_start(ap, id);                              \
    for (int k = 0; k < 4; k++) w[k] = va_arg(ap, uintptr_t);                  \
    va_end(ap);                                                                \
    return X##V(env, obj, id, w);                                              \
}                                                                              \
static R X##A(void *env, void *obj, int id, uint64_t *jv) {                   \
    uintptr_t w[4];                                                            \
    for (int k = 0; k < 4; k++) w[k] = jv ? (uintptr_t)jv[k] : 0;              \
    return X##V(env, obj, id, w);                                              \
}
JNI_VARIANTS(int,    CallStaticBooleanMethod)
JNI_VARIANTS(void *, CallStaticObjectMethod)
JNI_VARIANTS(int,    CallStaticIntMethod)
JNI_VARIANTS(void,   CallStaticVoidMethod)
#undef JNI_VARIANTS

static int GetVersion(void *env) { (void)env; return 0x00010006; }

/* The engine uses name-based JNI linkage (Java_com_activision_gw3_common_
 * GW3JNILib_*) and never calls this; kept only so the vtable slot resolves to
 * something that logs rather than to the jni_unimpl stub. */
static void RegisterNatives(void *env, int r1, void *r2) {
    (void)env; (void)r1;
    fprintf(stderr, "JNI: RegisterNatives ptr=%p (unexpected)\n", r2);
    uintptr_t *p = (uintptr_t *)r2;
    for (int i = 0; i < 64; i++) {
        const char *name = (const char *)p[i*3 + 0];
        const char *sig  = (const char *)p[i*3 + 1];
        void *fn         = (void *)p[i*3 + 2];
        if (!name) break;
        fprintf(stderr, "   native[%d] %s %s -> %p\n", i, name, sig ? sig : "", fn);
    }
}

static void *NewGlobalRef(void) { return (void *)0x42424242; }

/* Heuristic: is `p` a readable NUL-terminated ASCII string? Some engine call
 * sites pass a std::string/object pointer rather than a C string. */
static int looks_like_cstr(const char *p) {
    if (!p) return 0;
    for (int i = 0; i < 128; i++) {
        char c = p[i];              /* may fault -> caught by segv_handler */
        if (c == 0) return i > 0;
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return 0;
    }
    return 0;
}

static void *FindClass(void *env, char *name) {
    (void)env;
    if (looks_like_cstr(name))
        jni_tracef("JNI: FindClass %s\n", name);
    else
        jni_tracef("JNI: FindClass <obj %p>\n", (void *)name);
    static int dummy_cls;
    return &dummy_cls;
}

/* Our jstring representation IS the C string pointer (NewStringUTF/
 * GetStringUTFChars are identity), so a NULL jstring -> "" keeps callers that
 * pipe the result into strlen()/std::string from faulting when an unmapped or
 * unrecognised Call*Method returns NULL. */
static char *NewStringUTF(void *env, char *bytes) {
    (void)env;
    return bytes ? bytes : (char *)"";
}

static char *GetStringUTFChars(void *env, char *str, int *isCopy) {
    (void)env;
    if (isCopy) *isCopy = 0;
    return str ? str : (char *)"";
}

static int GetStringUTFLength(void *env, char *str) {
    (void)env;
    return str ? (int)strlen(str) : 0;
}

void *NVThreadGetCurrentJNIEnv(void) {
    return fake_env;
}

/* Populate the JNIEnv vtable. The whole table is first filled with jni_unimpl
 * (a safe no-op that logs the slot), then the known/needed slots are overlaid
 * with real handlers. This must run BEFORE so_initialize(): the engine's global
 * constructors call NVThreadGetCurrentJNIEnv during init_array and immediately
 * invoke methods through it, so a zeroed/partial vtable would crash. */
static void fill_env(void) {
    /* NOTE (2026-08-30): this vtable is the STANDARD JNINativeInterface layout,
     * not a custom one.  Every offset below is (jni.h index * 4) and each one
     * was cross-checked against a slot the engine actually calls.  The previous
     * table mislabelled the static-method block: 0x1C4 is GetStaticMethodID,
     * NOT CallBooleanMethod.  GW3's Java bridge (sub_0x23503c) calls
     *     env->FindClass(cls) ; env->GetStaticMethodID(cls, name, sig)
     * on every single invocation, so returning 0 there meant every static
     * method resolved to a null id and every subsequent
     * CallStaticObjectMethodV(id=0) came back empty -- the front end sat in
     * its "waiting on the Activity" poll forever behind a black screen. */
    fill_table_with_stub(fake_env, sizeof(fake_env));
    *(uintptr_t *)(fake_env + 0x00)  = (uintptr_t)fake_env;
    *(uintptr_t *)(fake_env + 0x10)  = (uintptr_t)GetVersion;      /*   4 */
    *(uintptr_t *)(fake_env + 0x18)  = (uintptr_t)FindClass;       /*   6 */
    *(uintptr_t *)(fake_env + 0x3C)  = (uintptr_t)ret0;            /*  15 ExceptionOccurred */
    *(uintptr_t *)(fake_env + 0x40)  = (uintptr_t)ret0;            /*  16 ExceptionDescribe */
    *(uintptr_t *)(fake_env + 0x44)  = (uintptr_t)ret0;            /*  17 ExceptionClear */
    *(uintptr_t *)(fake_env + 0x4C)  = (uintptr_t)ret0;            /*  19 PushLocalFrame */
    *(uintptr_t *)(fake_env + 0x50)  = (uintptr_t)ret0;            /*  20 PopLocalFrame */
    *(uintptr_t *)(fake_env + 0x54)  = (uintptr_t)NewGlobalRef;    /*  21 */
    *(uintptr_t *)(fake_env + 0x58)  = (uintptr_t)ret0;            /*  22 DeleteGlobalRef */
    *(uintptr_t *)(fake_env + 0x5C)  = (uintptr_t)ret0;            /*  23 DeleteLocalRef */
    *(uintptr_t *)(fake_env + 0x68)  = (uintptr_t)ret0;            /*  26 EnsureLocalCapacity */
    *(uintptr_t *)(fake_env + 0x7C)  = (uintptr_t)FindClass;       /*  31 GetObjectClass */

    /* instance methods: 33 GetMethodID, 34/35/36 Object, 37/38/39 Boolean,
     * 49/50/51 Int, 55/56/57 Float, 61/62/63 Void */
    *(uintptr_t *)(fake_env + 0x84)  = (uintptr_t)GetMethodID;
    *(uintptr_t *)(fake_env + 0x88)  = (uintptr_t)CallObjectMethodV;
    *(uintptr_t *)(fake_env + 0x8C)  = (uintptr_t)CallObjectMethodV;
    *(uintptr_t *)(fake_env + 0x90)  = (uintptr_t)CallObjectMethodV;
    *(uintptr_t *)(fake_env + 0x94)  = (uintptr_t)CallBooleanMethodV;
    *(uintptr_t *)(fake_env + 0x98)  = (uintptr_t)CallBooleanMethodV;
    *(uintptr_t *)(fake_env + 0x9C)  = (uintptr_t)CallBooleanMethodV;
    *(uintptr_t *)(fake_env + 0xC4)  = (uintptr_t)CallIntMethodV;
    *(uintptr_t *)(fake_env + 0xC8)  = (uintptr_t)CallIntMethodV;
    *(uintptr_t *)(fake_env + 0xCC)  = (uintptr_t)CallIntMethodV;
    *(uintptr_t *)(fake_env + 0xDC)  = (uintptr_t)CallFloatMethodV;
    *(uintptr_t *)(fake_env + 0xE0)  = (uintptr_t)CallFloatMethodV;
    *(uintptr_t *)(fake_env + 0xE4)  = (uintptr_t)CallFloatMethodV;
    *(uintptr_t *)(fake_env + 0xF4)  = (uintptr_t)CallVoidMethodV;
    *(uintptr_t *)(fake_env + 0xF8)  = (uintptr_t)CallVoidMethodV;
    *(uintptr_t *)(fake_env + 0xFC)  = (uintptr_t)CallVoidMethodV;

    /* fields: 94 GetFieldID, 95 GetObjectField, 96 GetBooleanField,
     * 100 GetIntField, 104 SetObjectField, 109 SetIntField.
     * viewInitGameConfig reads GameConfig via FIELDS (sig "I"), not methods —
     * which is why GetMethodID-at-0x178 appeared to work. */
    *(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
    *(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)CallObjectMethodV;
    *(uintptr_t *)(fake_env + 0x180) = (uintptr_t)CallBooleanMethodV;
    *(uintptr_t *)(fake_env + 0x190) = (uintptr_t)CallIntMethodV;
    *(uintptr_t *)(fake_env + 0x1A0) = (uintptr_t)ret0;
    *(uintptr_t *)(fake_env + 0x1B4) = (uintptr_t)ret0;

    /* static methods: 113 GetStaticMethodID, 114/115/116 Object,
     * 117/118/119 Boolean, 129/130/131 Int, 135/136/137 Float,
     * 141/142/143 Void.  THIS BLOCK IS THE FIX. */
    *(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
    *(uintptr_t *)(fake_env + 0x1C8) = (uintptr_t)CallStaticObjectMethodVa;
    *(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
    *(uintptr_t *)(fake_env + 0x1D0) = (uintptr_t)CallStaticObjectMethodA;
    *(uintptr_t *)(fake_env + 0x1D4) = (uintptr_t)CallStaticBooleanMethodVa;
    *(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
    *(uintptr_t *)(fake_env + 0x1DC) = (uintptr_t)CallStaticBooleanMethodA;
    *(uintptr_t *)(fake_env + 0x204) = (uintptr_t)CallStaticIntMethodVa;
    *(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
    *(uintptr_t *)(fake_env + 0x20C) = (uintptr_t)CallStaticIntMethodA;
    *(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallFloatMethodV;
    *(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallFloatMethodV;
    *(uintptr_t *)(fake_env + 0x224) = (uintptr_t)CallFloatMethodV;
    *(uintptr_t *)(fake_env + 0x234) = (uintptr_t)CallStaticVoidMethodVa;
    *(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
    *(uintptr_t *)(fake_env + 0x23C) = (uintptr_t)CallStaticVoidMethodA;

    /* static fields: 144 GetStaticFieldID, 145 Object, 146 Boolean, 150 Int */
    *(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
    *(uintptr_t *)(fake_env + 0x244) = (uintptr_t)CallStaticObjectMethodV;
    *(uintptr_t *)(fake_env + 0x248) = (uintptr_t)CallStaticBooleanMethodV;
    *(uintptr_t *)(fake_env + 0x258) = (uintptr_t)CallStaticIntMethodV;

    /* strings: 167 NewStringUTF, 168 GetStringUTFLength,
     * 169 GetStringUTFChars, 170 ReleaseStringUTFChars; 215 RegisterNatives */
    *(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
    *(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
    *(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
    *(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0;
    *(uintptr_t *)(fake_env + 0x35C) = (uintptr_t)RegisterNatives;
    /* byte[]: 171 GetArrayLength, 176 NewByteArray, 184 GetByteArrayElements,
     * 192 ReleaseByteArrayElements, 200 GetByteArrayRegion, 208 SetByteArrayRegion */
    *(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
    *(uintptr_t *)(fake_env + 0x2C0) = (uintptr_t)NewByteArray;
    *(uintptr_t *)(fake_env + 0x2E0) = (uintptr_t)GetByteArrayElements;
    *(uintptr_t *)(fake_env + 0x300) = (uintptr_t)ret0;
    *(uintptr_t *)(fake_env + 0x320) = (uintptr_t)GetByteArrayRegion;
    *(uintptr_t *)(fake_env + 0x340) = (uintptr_t)SetByteArrayRegion;
}

static int GetEnv(void *vm, void **env, int version) {
    (void)vm; (void)version;
    fill_env();
    *env = fake_env;
    return 0;
}

/* JavaVM invoke interface uses the STANDARD layout (only JNIEnv is custom):
 * index 4 (+0x10) AttachCurrentThread, +0x14 DetachCurrentThread,
 * +0x18 GetEnv, +0x1C AttachCurrentThreadAsDaemon. The engine's
 * "get JNIEnv for current thread" helper (sub_0x234fd0) calls +0x10 and
 * expects *p_env populated; leaving it stubbed returns NULL and the caller
 * (sub_0x23503c) then crashes doing env->FindClass. */
static int AttachCurrentThread(void *vm, void **p_env, void *thr_args) {
    (void)vm; (void)thr_args;
    fill_env();
    if (p_env) *p_env = fake_env;
    return 0; /* JNI_OK */
}

void jni_init(void) {
    fill_table_with_stub(fake_vm, sizeof(fake_vm));
    *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm;
    *(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)AttachCurrentThread;
    *(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0; /* DetachCurrentThread */
    *(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;
    *(uintptr_t *)(fake_vm + 0x1C) = (uintptr_t)AttachCurrentThread; /* ...AsDaemon */
    fill_env();   /* populate the JNIEnv vtable eagerly (see fill_env notes) */
}

void jni_load(void) {
    /* Un-pause the game (it starts paused by default) */
    int *paused = (int *)so_symbol(&gw3_mod, "IsAndroidPaused");
    if (paused) *paused = 0;

    /* JNI_OnLoad runs the engine's C++ static constructors. */
    int (*JNI_OnLoad)(void *vm, void *reserved) =
        (void *)so_symbol(&gw3_mod, "JNI_OnLoad");
    if (!JNI_OnLoad) {
        fprintf(stderr, "jni_load: JNI_OnLoad not found\n");
        return;
    }
    fprintf(stderr, "jni_load: calling JNI_OnLoad\n"); fflush(stderr);
    JNI_OnLoad(fake_vm, NULL);
    fprintf(stderr, "jni_load: JNI_OnLoad returned\n"); fflush(stderr);

    {
        /* GW3's engine does NOT use RegisterNatives: it relies on Android's
         * name-based native linkage (Java_com_activision_gw3_common_GW3JNILib_*).
         * There is no Java layer here, so we drive the whole Android
         * GLSurfaceView lifecycle by hand, faking the `thiz` / `config` /
         * `AssetManager` objects the GW3Activity would supply.
         *
         * Discovered call graph (capstone on libgwnext.so):
         *   viewInitGameConfig  -> reads GameConfig getters (res, tvDevice)
         *   onAppCreated        -> caches JNIEnv
         *   setAssetManager     -> NewGlobalRef(am) into a global
         *   set{DeviceName,ScreenSizeInches,DeviceScreenSize,AppWindowSize}
         *                       -> store into globals (strings via env+0x2a4)
         *   set{GameFilesDir,PrivateFilesDir} -> strlcpy into static buffers
         *   viewOnSurfaceCreated-> empty
         *   viewOnInit          -> sub_0x29f050: the real native init
         *   viewOnSurfaceChanged(w,h) -> sub_0x29f09c: viewport/resize
         *   viewOnDrawFrame     -> sub_0x29f160: per-frame tick (call in a loop)
         * The engine imports only eglGetProcAddress and assumes a GL context is
         * already current on the calling thread -- SDL gave us one at startup. */
        fprintf(stderr, "jni_load: RegisterNatives not called; driving GW3JNILib lifecycle\n");
        fflush(stderr);

        #define GW3SYM(n) ((void *)so_symbol(&gw3_mod, \
            "Java_com_activision_gw3_common_GW3JNILib_" n))
        void (*fn_onAppCreated)(void *, void *)                 = GW3SYM("onAppCreated");
        void (*fn_setAssetManager)(void *, void *, void *)      = GW3SYM("setAssetManager");
        void (*fn_setDeviceName)(void *, void *, void *)        = GW3SYM("setDeviceName");
        void (*fn_setScreenSizeInches)(void *, void *, int)     = GW3SYM("setScreenSizeInches"); /* soft-float: bits in r2 */
        void (*fn_setDeviceScreenSize)(void *, void *, int, int)= GW3SYM("setDeviceScreenSize");
        void (*fn_setAppWindowSize)(void *, void *, int, int)   = GW3SYM("setAppWindowSize");
        void (*fn_setGameFilesDir)(void *, void *, void *)      = GW3SYM("setGameFilesDir");
        void (*fn_setPrivateFilesDir)(void *, void *, void *)   = GW3SYM("setPrivateFilesDir");
        void (*fn_setNoJoysticks)(void *, void *, int)          = GW3SYM("setNoJoysticks");
        void (*fn_setJoystickConnected)(void *, void *, int, int) = GW3SYM("setJoystickConnected"); /* 1.3.0 only */
        void (*fn_viewInitGameConfig)(void *, void *, void *)   = GW3SYM("viewInitGameConfig");
        void (*fn_viewOnSurfaceCreated)(void *, void *)         = GW3SYM("viewOnSurfaceCreated");
        void (*fn_viewOnSurfaceChanged)(void *, void *, int, int)= GW3SYM("viewOnSurfaceChanged");
        void (*fn_viewOnInit)(void *, void *)                   = GW3SYM("viewOnInit");
        void (*fn_viewOnResume)(void *, void *)                 = GW3SYM("viewOnResume");
        void (*fn_viewOnDrawFrame)(void *, void *)              = GW3SYM("viewOnDrawFrame");
        #undef GW3SYM

        if (!fn_viewInitGameConfig || !fn_viewOnInit || !fn_viewOnDrawFrame) {
            fprintf(stderr, "jni_load: required GW3JNILib symbols missing "
                    "(cfg=%p init=%p draw=%p)\n", (void *)fn_viewInitGameConfig,
                    (void *)fn_viewOnInit, (void *)fn_viewOnDrawFrame);
            return;
        }

        static int fake_thiz, fake_config, fake_am; /* non-NULL fake Java objects */
        g_tv_mode = fn_setJoystickConnected && !getenv("GW3_PHONE");
        fprintf(stderr, "GW3: game %s, %s UI\n", fn_setJoystickConnected ? "1.3.0 Evolved" : "1.0.x",
                g_tv_mode ? "TV/console" : "phone");
        int inches_bits;
        { float f = 5.0f; memcpy(&inches_bits, &f, sizeof f); }

        /* Keep the SDL GL context current on this thread: we drive rendering
         * from here (no separate render thread like the GTA port had). */
        SDL_GL_MakeCurrent(g_window, g_gl_ctx);

        #define STEP(label, call) do { \
            fprintf(stderr, "GW3 lifecycle: " label "\n"); fflush(stderr); \
            call; \
            fprintf(stderr, "GW3 lifecycle: " label " ok\n"); fflush(stderr); \
        } while (0)

        if (fn_onAppCreated)        STEP("onAppCreated",       fn_onAppCreated(fake_env, &fake_thiz));
        if (fn_setAssetManager)     STEP("setAssetManager",    fn_setAssetManager(fake_env, &fake_thiz, &fake_am));
        if (fn_setDeviceName)       STEP("setDeviceName",      fn_setDeviceName(fake_env, &fake_thiz, (void *)"R36S"));
        if (fn_setScreenSizeInches) STEP("setScreenSizeInches", fn_setScreenSizeInches(fake_env, &fake_thiz, inches_bits));
        fprintf(stderr, "GW3 lifecycle: feeding panel size %dx%d\n", g_screen_w, g_screen_h);
        if (fn_setDeviceScreenSize) STEP("setDeviceScreenSize", fn_setDeviceScreenSize(fake_env, &fake_thiz, g_screen_w, g_screen_h));
        if (fn_setAppWindowSize)    STEP("setAppWindowSize",   fn_setAppWindowSize(fake_env, &fake_thiz, g_screen_w, g_screen_h));
        if (fn_setGameFilesDir)     STEP("setGameFilesDir",    fn_setGameFilesDir(fake_env, &fake_thiz, (void *)data_path_slash()));
        if (fn_setPrivateFilesDir)  STEP("setPrivateFilesDir", fn_setPrivateFilesDir(fake_env, &fake_thiz, (void *)data_path_slash()));
        /* g_NoJoypads is the *count* of connected pads (see main.c note).
         * setNoJoysticks(1) sets it; _Z12GetNoJoypadsv is also hooked ->1 in
         * patch_gw3.  Real pad state is fed per-frame by pump_joy_input(). */
        if (fn_setNoJoysticks)      STEP("setNoJoysticks",     fn_setNoJoysticks(fake_env, &fake_thiz, 1));
        if (fn_setJoystickConnected) STEP("setJoystickConnected", fn_setJoystickConnected(fake_env, &fake_thiz, 0, 1));
        {
            int *p_nojoy = (int *)so_symbol(&gw3_mod, "g_NoJoypads");
            if (p_nojoy) { *p_nojoy = 1; fprintf(stderr, "GW3 input: g_NoJoypads(count) @%p -> 1\n", (void *)p_nojoy); }
        }
        fflush(stderr);

        STEP("viewInitGameConfig", fn_viewInitGameConfig(fake_env, &fake_thiz, &fake_config));
        if (fn_viewOnSurfaceCreated) STEP("viewOnSurfaceCreated", fn_viewOnSurfaceCreated(fake_env, &fake_thiz));
        STEP("viewOnInit", fn_viewOnInit(fake_env, &fake_thiz));
        if (fn_viewOnSurfaceChanged) STEP("viewOnSurfaceChanged", fn_viewOnSurfaceChanged(fake_env, &fake_thiz, g_screen_w, g_screen_h));
        if (fn_viewOnResume) STEP("viewOnResume", fn_viewOnResume(fake_env, &fake_thiz));
        #undef STEP

        fprintf(stderr, "GW3 lifecycle: entering draw loop\n"); fflush(stderr);
        extern int ProcessEvents(void);
        unsigned long frame = 0;
        for (;;) {
            if (ProcessEvents()) break;
            pump_joy_input();
            jni_deliver_cloud_read();
            fn_viewOnDrawFrame(fake_env, &fake_thiz);
            qol_draw(g_screen_w, g_screen_h);
            if (frame == 300) {
                /* one-shot render probe: is FBO 0 bound, is the backbuffer non-black? */
                GLint fbo = -1, vp[4] = {0}; GLenum err = glGetError();
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
                glGetIntegerv(GL_VIEWPORT, vp);
                unsigned char px[4] = {9,9,9,9};
                glReadPixels(vp[2] ? vp[2]/2 : 512, vp[3] ? vp[3]/2 : 384,
                             1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                fprintf(stderr,
                    "GL PROBE f300: err=0x%04x fbo=%d viewport=[%d %d %d %d] "
                    "centerpx=%02x%02x%02x%02x readErr=0x%04x\n",
                    err, fbo, vp[0], vp[1], vp[2], vp[3],
                    px[0], px[1], px[2], px[3], glGetError());
                fflush(stderr);
            }
            SDL_GL_SwapWindow(g_window);
            if ((frame % 120) == 0) {
                fprintf(stderr, "GW3 draw loop: frame %lu\n", frame);
                fflush(stderr);
            }
            frame++;
        }
        fprintf(stderr, "GW3 lifecycle: draw loop exited\n"); fflush(stderr);
        SDL_Quit();
    }
}
