/* main.c -- Geometry Wars 3: Dimensions loader for R36S / NextOS (Linux handhelds) */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <fenv.h>
#include <setjmp.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <ucontext.h>
#include <execinfo.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <zlib.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include "config.h"
#include "so_util.h"
#include "jni_patch.h"
#include "opengl_patch.h"
#include "aasset_patch.h"
#include "fmod_patch.h"
#include "qol.h"

/* ── Globals shared with other modules ──────────────────────────────────── */

so_module           gw3_mod;
SDL_Window         *g_window  = NULL;
SDL_GLContext       g_gl_ctx  = NULL;
SDL_GameController *g_gamepad = NULL;
int                 g_gamepad_buttons = 0;
float               g_gamepad_axis[6] = { 0 };
char                g_data_path[512]  = DATA_PATH;

/* Real panel dimensions.  Seeded from config.h, overwritten in main() with the
 * SDL window's actual size once it is created FULLSCREEN_DESKTOP.  jni_patch.c
 * feeds these to the engine (setDeviceScreenSize / setAppWindowSize / the
 * GameConfig width/height getters) so it renders at native res / aspect for
 * whatever panel it lands on (1024x768 Ultra X, 720x720 Ultra, ...). */
int                 g_screen_w = SCREEN_W;
int                 g_screen_h = SCREEN_H;

/* FMOD companion modules, loaded from the data dir (see fmod_patch.c). */
so_module fm_mod, fme_mod;
int g_fmod_ok = 0;   /* set by fmod_preload(): real FMOD libs loaded */

/* AAssetManager (NDK) shim implemented in aasset_patch.c. Opaque types are
 * passed through as void* here to avoid pulling in Android headers. */
extern void *AAssetManager_fromJava(void *env, void *jassetmanager);
extern void  *AAssetManager_open(void *mgr, const char *filename, int mode);
extern int    AAsset_read(void *asset, void *buf, int count);
extern void   AAsset_close(void *asset);
extern long   AAsset_getLength(void *asset);
extern long   AAsset_getRemainingLength(void *asset);
extern long   AAsset_seek(void *asset, long offset, int whence);

/* ── C++ ABI symbols ─────────────────────────────────────────────────────── */
extern int  __cxa_atexit(void (*)(void *), void *, void *) __attribute__((weak));
extern void __cxa_finalize(void *)                         __attribute__((weak));

/* __cxa_guard: replace bionic libc++ implementation with a simple GCC-compatible one.
 * Guard object layout: byte 0 = initialized flag.
 * Returns 1 (proceed) if not yet initialized, 0 if done. */
static int  __cxa_guard_acquire_impl(long *g) { return (*(char*)g == 0); }
static void __cxa_guard_release_impl(long *g) { *(char*)g = 1; }
static void __cxa_guard_abort_impl  (long *g) { (void)g; }

/* ── glibc gettid wrapper (kernel syscall) ───────────────────────────────── */
static pid_t _gettid(void) { return (pid_t)syscall(SYS_gettid); }

/* clock_gettime, __clock_gettime64, and clock_gettime64_safe are defined in
 * clock_fix.c (separate TU that avoids <time.h>'s __asm__ alias which would
 * cause duplicate symbols when both are defined in the same translation unit) */
extern int clock_gettime(clockid_t, struct timespec *);
extern int __clock_gettime64(clockid_t, void *);
extern int clock_gettime64_safe(clockid_t, void *);
extern int gettimeofday64_safe(void *, void *);
extern int gettimeofday_safe(void *, void *);

/* ── isfinite / signbit: glibc provides these as macros, expose functions ── */
static int _isfinite(double d) { return isfinite(d); }
static int _signbit(double d)  { return signbit(d); }

/* ── Stub helpers ────────────────────────────────────────────────────────── */

static int  ret0(void)  { return 0; }
static int  ret1(void)  { return 1; }

/* ── GW3 joypad wiring ────────────────────────────────────────────────────
 * Reverse-engineered from libgwnext.so (2026-08-30):
 *
 *  g_JoypadStates (dynsym 0x9240a8, 144 B) = 4 joypads x 9 words:
 *      word 0      : button bitmask
 *      words 1..8  : float axis[0..7]  (order: LX LY RX RY LTrig RTrig HatY HatX)
 *  _Z16GetJoypadButtonsi / _Z13GetJoypadAxisii read that array directly;
 *  _Z16JoyButtonPressedi14E_JoypadButton == (mask >> btn) & 1.
 *
 *  E_JoypadButton bit order (from the _Z21GetJoypadValueForAxisi9InputAxis
 *  jump table @0x451214 -> JoyButtonPressed(joy, N); positions proven,
 *  face/trigger/start-back labels are conventional for that layout):
 *      0 A   1 B   2 X   3 Y   4 L1  5 R1  6 L2/LTrig  7 R2/RTrig
 *      8 DpadUp  9 DpadDown  10 DpadLeft  11 DpadRight  12 Start  13 Back
 *
 *  Detection: _ZN12UserControls27GetMasterUserControllerTypeEv returns the
 *  User-0 controller type -- 0 = physical joypad (Lua IsUsingJoypad true only
 *  here), 3 = on-screen virtual stick (IsControllerVStick), 5 = none.
 *  _Z12GetNoJoypadsv is the *count* of connected pads (used as a loop bound).
 *  _ZN11InputDevice6UpdateEv @0x451dcc pops the blocking "reconnect
 *  controller" popup (text 0x1aa) whenever type==0 && count==0 in active play,
 *  so we pin count=1 and type=0.
 */
extern int   g_gamepad_buttons;   /* SDL_CONTROLLER_BUTTON_* mask, see ProcessEvents */
extern float g_gamepad_axis[6];

/* Raw SDL state -> engine bits.  The R36S labels its face buttons the
 * Nintendo way (A right, B bottom, X top, Y left) while SDL names them by
 * position (A = bottom), so by default the engine's A ("confirm") is taken
 * from the button LABELLED A and B ("back") from the one labelled B; same for
 * X/Y.  Options > Graphics > BUTTONS (or the port menu) switches to the
 * positional Xbox layout. */
static int raw_engine_mask(void) {
    int s = g_gamepad_buttons, m = 0;
    int nintendo = qol_nintendo_layout();
    int a = nintendo ? 0x002 : 0x001, b = nintendo ? 0x001 : 0x002;
    int x = nintendo ? 0x008 : 0x004, y = nintendo ? 0x004 : 0x008;
    if (s & a)      m |= 1 << 0;   /* A              */
    if (s & b)      m |= 1 << 1;   /* B              */
    if (s & x)      m |= 1 << 2;   /* X              */
    if (s & y)      m |= 1 << 3;   /* Y              */
    if (s & 0x040)  m |= 1 << 4;   /* L1 / LShoulder */
    if (s & 0x080)  m |= 1 << 5;   /* R1 / RShoulder */
    if (g_gamepad_axis[4] > 0.30f) m |= 1 << 6;  /* L2 (analog trigger -> digital) */
    if (g_gamepad_axis[5] > 0.30f) m |= 1 << 7;  /* R2 (analog trigger -> digital) */
    if (s & 0x100)  m |= 1 << 8;   /* DPad Up    */
    if (s & 0x200)  m |= 1 << 9;   /* DPad Down   */
    if (s & 0x400)  m |= 1 << 10;  /* DPad Left  */
    if (s & 0x800)  m |= 1 << 11;  /* DPad Right */
    if (s & 0x010)  m |= 1 << 12;  /* Start */
    if (s & 0x020)  m |= 1 << 13;  /* Back / Select */
    return m;
}

/* This frame's engine-side pad state, latched once per frame by
 * gw3_input_frame() so the joypad array and the GetJoypad* hooks agree. */
static int   g_eng_mask;
static float g_eng_axis[8];

void gw3_input_frame(void) {
    int m = raw_engine_mask();
    float ax[8] = {0};
    /* Sticks (0..3) and triggers (4/5) pass through raw: the odroidgo3-joypad
     * kernel driver already does centre calibration / dead-zone, so a second
     * dead-zone in here only eats range and (if per-axis) makes the stick snap
     * to the cardinals.  Tune stick feel via OS joystick calibration.
     * Axes 6/7 (Hat) stay 0: the D-pad goes through button bits 8..11. */
    for (int i = 0; i < 6; i++) ax[i] = g_gamepad_axis[i];
    /* The 1.3.0 console menus only follow the left stick, so the D-pad is
     * mirrored onto it (in a level the left stick is movement too). */
    ax[0] += (float)(!!(m & 1 << 11) - !!(m & 1 << 10));
    ax[1] += (float)(!!(m & 1 << 9)  - !!(m & 1 << 8));
    for (int i = 0; i < 2; i++) ax[i] = ax[i] > 1.0f ? 1.0f : ax[i] < -1.0f ? -1.0f : ax[i];
    if (qol_input(&m)) memset(ax, 0, sizeof ax);   /* port menu is up: the game sees nothing */
    g_eng_mask = m;
    memcpy(g_eng_axis, ax, sizeof ax);
}

int gw3_engine_button_mask(void) { return g_eng_mask; }

float gw3_engine_axis(int a) { return (a >= 0 && a < 8) ? g_eng_axis[a] : 0.0f; }

/* Pin User 0 to "physical joypad, one pad connected". */
static int GetNoJoypads_hook(void) { return 1; }
static int GetMasterUserControllerType_hook(void) { return 0; }

static int GetJoypadButtons_hook(int joy) {
    return joy == 0 ? gw3_engine_button_mask() : 0;
}
static int GetJoypadAxis_hook(int joy, int axis) {
    float v = (joy == 0 && axis >= 0 && axis <= 7) ? gw3_engine_axis(axis) : 0.0f;
    int bits; memcpy(&bits, &v, sizeof bits);
    return bits; /* soft-float ABI: float result returned in r0 */
}

static volatile int g_malloc_count = 0;
static void *malloc_debug(size_t n) {
    return malloc(n);
}

/* ── ARM EABI memory helpers: argument order differs from glibc ──────────── */
/* __aeabi_memset(dst, n, c) — note: n and c are SWAPPED vs memset(dst,c,n) */
static void __aeabi_memset_impl(void *dst, size_t n, int c)  { memset(dst, c, n); }
/* __aeabi_memclr(dst, n) — zero n bytes */
static void __aeabi_memclr_impl(void *dst, size_t n)         { memset(dst, 0, n); }

/* libgcc's 64-bit divmod helper — libgwnext imports it by name but it is a
 * hidden symbol in our own libgcc, so dlsym can't see it. Reimplement (the / and
 * % below pull __divdi3/__moddi3 from our statically-linked libgcc). */
long long __gnu_ldivmod_helper(long long a, long long b, long long *rem) {
    long long q = a / b;
    if (rem) *rem = a - q * b;
    return q;
}
unsigned long long __gnu_uldivmod_helper(unsigned long long a, unsigned long long b,
                                         unsigned long long *rem) {
    unsigned long long q = a / b;
    if (rem) *rem = a - q * b;
    return q;
}

/* ── bionic-compatible setjmp/longjmp ───────────────────────────────────────
 * The engine + its bundled libpng/zlib were built against bionic's <setjmp.h>,
 * whose arm jmp_buf is a 256-byte long[64] with NO signal-mask area. glibc's
 * setjmp() is __sigsetjmp(env,1): it runs rt_sigprocmask and writes the core
 * regs PLUS __mask_was_saved + a 128-byte __saved_mask — ~390 bytes total — so
 * feeding it a bionic-sized buffer smashes whatever sits just past it (in
 * libpng's png_safe_execute that is the saved fp/lr, which get zeroed → the
 * function returns to address 0). Provide a small, matched pair that saves only
 * the callee-saved GPRs, sp, lr and the callee-saved VFP regs (~104 bytes). */
__attribute__((naked)) static int  bionic_setjmp(void *buf) {
    __asm__ volatile(
        ".syntax unified\n"
        "stmia  r0!, {r4-r11}\n"
        "str    sp,  [r0], #4\n"
        "str    lr,  [r0], #4\n"
        "vstmia r0,  {d8-d15}\n"
        "movs   r0,  #0\n"
        "bx     lr\n");
}
__attribute__((naked)) static void bionic_longjmp(void *buf, int val) {
    __asm__ volatile(
        ".syntax unified\n"
        "ldmia  r0!, {r4-r11}\n"
        "ldr    sp,  [r0], #4\n"
        "ldr    lr,  [r0], #4\n"
        "vldmia r0,  {d8-d15}\n"
        "movs   r0,  r1\n"
        "it     eq\n"
        "moveq  r0,  #1\n"
        "bx     lr\n");
}

/* ── FMOD .so low-level deps not provided by glibc ───────────────────────── */

/* FMOD's OpenSL output does dlopen("libOpenSLES.so"); hand it the SDL2-backed
 * fake from fmod_opensl.c. Everything else passes through to the real loader. */
static void *g_opensl_handle;   /* address is the sentinel handle value */

static void *fmod_dlopen(const char *name, int flags) {
    if (name && strstr(name, "OpenSLES")) {
        fprintf(stderr, "fmod: dlopen(\"%s\") -> SDL2 OpenSL shim\n", name);
        return &g_opensl_handle;
    }
    return dlopen(name, flags);
}
static void *fmod_dlsym(void *handle, const char *sym) {
    if (handle == &g_opensl_handle) return fmod_opensl_sym(sym);
    return dlsym(handle, sym);
}
static int fmod_dlclose(void *handle) {
    if (handle == &g_opensl_handle) return 0;
    return dlclose(handle);
}

/* bionic spellings glibc lacks */
static int __android_log_write_shim(int prio, const char *tag, const char *msg) {
    (void)prio;
#ifdef DEBUG
    fprintf(stderr, "[%s] %s\n", tag ? tag : "?", msg ? msg : "");
#else
    (void)tag; (void)msg;
#endif
    return 0;
}
static int *__errno_shim(void) { return &errno; }

/* ── Android log → stderr ────────────────────────────────────────────────── */

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
#else
    (void)prio; (void)tag; (void)fmt;
#endif
    return 0;
}

/* ── Screen size (hooked into .so) ───────────────────────────────────────── */

int OS_ScreenGetWidth(void)  { return g_screen_w; }
int OS_ScreenGetHeight(void) { return g_screen_h; }

/* ── Bionic pthread/semaphore ABI shims ──────────────────────────────────────
 * On Android bionic (32-bit): mutex=4 bytes, cond=4 bytes, sem=4 bytes.
 * On glibc (32-bit):          mutex=24 bytes, cond=48 bytes, sem=16 bytes.
 * pthread_attr_t: bionic=24 bytes, glibc=36 bytes.
 *
 * Strategy for mutex/cond/sem: store a heap pointer to a real glibc object in
 * the game's 4-byte bionic slot.  Lazy-init handles zero-initialised globals.
 * Strategy for attr: use our own 24-byte layout, ignore glibc's larger struct.
 */

/* NEVER hand the caller's attr to glibc: `a` points at bionic's (or our own
 * 24-byte) attr layout, which glibc reinterprets as its own — that hands it a
 * garbage protocol/prioceiling and PTHREAD_PRIO_PROTECT with an out-of-range
 * ceiling, which aborts on first lock:
 *   tpp.c:82: __pthread_tpp_change_priority: Assertion `new_prio == -1 || ...'
 * pthread_mutexattr_settype is a no-op here so we cannot know the requested
 * type either; RECURSIVE is the safe superset (a non-recursive user is
 * unaffected, a recursive one would otherwise self-deadlock). */
static int pthread_mutex_init_fake(pthread_mutex_t **m,
                                    const pthread_mutexattr_t *a) {
    static pthread_mutexattr_t rec_attr;
    static int rec_ready = 0;
    (void)a;
    if (!rec_ready) {
        pthread_mutexattr_init(&rec_attr);
        pthread_mutexattr_settype(&rec_attr, PTHREAD_MUTEX_RECURSIVE);
        rec_ready = 1;
    }
    pthread_mutex_t *real = calloc(1, sizeof(pthread_mutex_t));
    pthread_mutex_init(real, &rec_attr);
    *m = real;
    return 0;
}
static int pthread_mutex_destroy_fake(pthread_mutex_t **m) {
    if (*m) { pthread_mutex_destroy(*m); free(*m); *m = NULL; }
    return 0;
}
static int pthread_mutex_lock_fake(pthread_mutex_t **m) {
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_mutex_lock(*m);
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **m) {
    if (!*m) return 0;
    return pthread_mutex_unlock(*m);
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **m) {
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_mutex_trylock(*m);
}

static int pthread_cond_init_fake(pthread_cond_t **c,
                                   const pthread_condattr_t *a) {
    pthread_cond_t *real = calloc(1, sizeof(pthread_cond_t));
    pthread_cond_init(real, a);
    *c = real;
    return 0;
}
static int pthread_cond_destroy_fake(pthread_cond_t **c) {
    if (*c) { pthread_cond_destroy(*c); free(*c); *c = NULL; }
    return 0;
}
static int pthread_cond_wait_fake(pthread_cond_t **c, pthread_mutex_t **m) {
    if (!*c) pthread_cond_init_fake(c, NULL);
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_cond_wait(*c, *m);
}
static int pthread_cond_timedwait_fake(pthread_cond_t **c, pthread_mutex_t **m,
                                        const struct timespec *t) {
    if (!*c) pthread_cond_init_fake(c, NULL);
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_cond_timedwait(*c, *m, t);
}
static int pthread_cond_signal_fake(pthread_cond_t **c) {
    if (*c) return pthread_cond_signal(*c);
    return 0;
}
static int pthread_cond_broadcast_fake(pthread_cond_t **c) {
    if (*c) return pthread_cond_broadcast(*c);
    return 0;
}

static int sem_init_fake(sem_t **s, int pshared, unsigned int value) {
    sem_t *real = calloc(1, sizeof(sem_t));
    sem_init(real, pshared, value);
    *s = real;
    return 0;
}
static int sem_destroy_fake(sem_t **s) {
    if (*s) { sem_destroy(*s); free(*s); *s = NULL; }
    return 0;
}
static int sem_wait_fake(sem_t **s) {
    if (!*s) sem_init_fake(s, 0, 0);
    return sem_wait(*s);
}
static int sem_post_fake(sem_t **s) {
    if (!*s) sem_init_fake(s, 0, 0);
    return sem_post(*s);
}
static int sem_trywait_fake(sem_t **s) {
    if (!*s) return EAGAIN;
    return sem_trywait(*s);
}
static int sem_getvalue_fake(sem_t **s, int *val) {
    if (!*s) { if (val) *val = 0; return 0; }
    return sem_getvalue(*s, val);
}

/* pthread_attr_t: bionic layout (24 bytes) — store only what we need */
typedef struct {
    uint32_t flags;
    void    *stack_base;
    size_t   stack_size;
    size_t   guard_size;
    int32_t  sched_policy;
    int32_t  sched_priority;
} bionic_attr_t;

static int pthread_attr_init_fake(bionic_attr_t *a) {
    memset(a, 0, sizeof(*a));
    a->guard_size = 4096;
    return 0;
}
static int pthread_attr_destroy_fake(bionic_attr_t *a)          { (void)a; return 0; }
static int pthread_attr_setstacksize_fake(bionic_attr_t *a, size_t s) {
    a->stack_size = s; return 0;
}
static int pthread_attr_getstacksize_fake(bionic_attr_t *a, size_t *s) {
    *s = a->stack_size; return 0;
}
static int pthread_attr_getstack_fake(bionic_attr_t *a, void **base, size_t *s) {
    *base = a->stack_base; *s = a->stack_size; return 0;
}
static int pthread_attr_setschedparam_fake(bionic_attr_t *a,
                                            const struct sched_param *p) {
    a->sched_priority = p->sched_priority; return 0;
}
static int pthread_attr_getschedparam_fake(bionic_attr_t *a, struct sched_param *p) {
    p->sched_priority = a->sched_priority; return 0;
}

/* ── Real pthread_create (RTLD_NEXT avoids recursion when we define the symbol) */
static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *,
                                   void *(*)(void *), void *) = NULL;
static void init_real_pthread_create(void) {
    if (!real_pthread_create)
        real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
}

/* Global pthread_create — intercepts thread creation from ALL loaded shared libs.
 * The executable's strong definition preempts libpthread's for every .so loaded
 * with RTLD_GLOBAL (which includes system libopenal.so). Lets us catch any
 * library that tries to spawn a thread with a NULL start_routine. */
int pthread_create(pthread_t *tidp, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    init_real_pthread_create();
    /* Use volatile to prevent -O2 from eliminating the NULL check based on
     * the nonnull prototype attribute. */
    void *(*volatile sr)(void *) = start_routine;
    pthread_t *volatile tp = tidp;
    if (!sr) {
        if (tp) *tp = 0;
        return 0;
    }
    return real_pthread_create(tidp, attr, sr, arg);
}

/* ── Bionic TSD stubs ────────────────────────────────────────────────────────
 * The PSVita port stubs all TSD operations as no-ops and that works fine.
 * Our real bionic→glibc key mapping caused pthread_kill to be called with
 * a corrupted TID when keys or their values got out of sync with glibc
 * internals.  Match the PSVita approach: key_create/delete/get/set are all
 * ret0 — NVThreadGetCurrentJNIEnv() is already hooked directly so the game's
 * JNI env lookup never needs TSD. */

/* Global pthread_cancel interceptor — the game does not import pthread_cancel,
 * but SDL2/OpenAL may call it to stop their internal threads.  Log it so we
 * can identify which thread is being cancelled and what its pthread_t is. */
int pthread_cancel(pthread_t thread) {
    static int (*real_pc)(pthread_t) = NULL;
    if (!real_pc)
        real_pc = dlsym(RTLD_NEXT, "pthread_cancel");
    return real_pc(thread);
}

/* pthread_create_fake: called from libgwnext.so's GOT (bionic ABI, pointer-redirect
 * attr).  Uses a trampoline to log thread start/stop and handles NULL guards. */
typedef struct { void *(*func)(void *); void *arg; } pt_tramp_t;

static void *pthread_tramp(void *p) {
    pt_tramp_t *t = p;
    void *(*f)(void *) = t->func;
    void *a = t->arg;
    free(t);
    return f(a);
}

static int pthread_create_fake(pthread_t *tidp, bionic_attr_t *attr,
                                void *func, void *arg) {
    (void)attr;
    if (!func) {
        if (tidp) *tidp = 0;
        return 0;
    }
    init_real_pthread_create();
    pt_tramp_t *t = malloc(sizeof(*t));
    t->func = (void *(*)(void *))func;
    t->arg  = arg;
    pthread_t tid;
    int r = real_pthread_create(&tid, NULL, pthread_tramp, t);
    if (tidp) *tidp = tid;
    return r;
}

/* ── stat hook: game checks mtime at statbuf+0x50 (Android struct layout) ── */

static int stat_hook(const char *path, void *statbuf) {
    struct stat st;
    int r = stat(path, &st);
    if (r == 0)
        *(int *)((char *)statbuf + 0x50) = (int)st.st_mtime;
    return r;
}

/* ── ctype / stdio ABI compatibility ─────────────────────────────────────── */

/* Android libc exposes these as pointers; glibc does too but at different
 * symbol names.  We provide matching data so the game finds valid tables. */

static const short C_tolower_tab[257] = {
    -1,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
    0x40,'a','b','c','d','e','f','g',
    'h','i','j','k','l','m','n','o',
    'p','q','r','s','t','u','v','w',
    'x','y','z',0x5b,0x5c,0x5d,0x5e,0x5f,
    0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
    0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
    0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
    0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,
    0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
    0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,
    0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,
    0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
    0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,
    0xd0,0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,
    0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,
    0xe0,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
    0xe8,0xe9,0xea,0xeb,0xec,0xed,0xee,0xef,
    0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
    0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff,
};
static const short *tolower_tab_ptr = C_tolower_tab;

/* Android __sF is an array of embedded bionic FILE structs (~84 bytes each).
 * Allocate enough space so __sF[1] (stdout) lands inside our buffer.
 * resolve_stream() maps bionic-fake addresses back to real glibc streams. */
#define BIONIC_FILE_SIZE 84
static char  sF_fake[3 * BIONIC_FILE_SIZE];
static FILE *stderr_fake;
static int   stack_chk_guard_fake = 0x42424242;

/* ctype_ pointer: provided via android_ctype_table below */

/* ── ProcessEvents: called once per frame by the game ────────────────────── */

int ProcessEvents(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            return 1; /* signal exit */
    }

    /* Update gamepad state */
    if (g_gamepad) {
        int mask = 0;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_A))         mask |= 0x001;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_B))         mask |= 0x002;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_X))         mask |= 0x004;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_Y))         mask |= 0x008;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_START))     mask |= 0x010;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_BACK))      mask |= 0x020;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  mask |= 0x040;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= 0x080;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP))   mask |= 0x100;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) mask |= 0x200;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mask |= 0x400;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))mask |= 0x800;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_LEFTSTICK)) mask |= 0x1000;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSTICK))mask |= 0x2000;

        /* Start+Select held together → clean exit */
        if ((mask & 0x010) && (mask & 0x020)) {
            fprintf(stderr, "Start+Select: exiting\n");
            fflush(stderr);
            SDL_Quit();
            exit(0);
        }

        g_gamepad_buttons = mask;

        /* Axes: normalise SDL's -32768..32767 to -1..1 */
        g_gamepad_axis[0] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX)  / 32767.0f;
        g_gamepad_axis[1] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY)  / 32767.0f;
        g_gamepad_axis[2] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        g_gamepad_axis[3] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
        /* Triggers: 0..32767 → 0..1 */
        g_gamepad_axis[4] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  / 32767.0f;
        g_gamepad_axis[5] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
    }

    return 0; /* 1 = exit */
}

/* ── __assert2 (Android assertion handler) ───────────────────────────────── */
static void __assert2_impl(const char *file, int line,
                            const char *func, const char *expr) {
    fprintf(stderr, "ASSERT FAIL: %s:%d %s(): %s\n", file, line, func, expr);
    fflush(stderr);
    /* Don't call real abort() — that raises SIGABRT through glibc internals
     * bypassing our raise_hook. Just hang so we can see the log. */
    for (;;) usleep(1000000);
}

static void abort_hook(void) {
    write(2, "ABORT_HOOK\n", 11);  /* confirm hook fires */
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "abort() intercepted from game (caller=%p):\n",
            __builtin_return_address(0));
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  bt[%d] %s\n", i, syms ? syms[i] : "?");
    free(syms);
    fflush(stderr);
    /* Swallow — loop so execution doesn't continue off the end */
    for (;;) usleep(1000000);
}

/* Intercept raise() from libgwnext.so: log and swallow.
 * bionic's abort() calls raise(SIGABRT); some internal crash paths call raise(SIGSEGV).
 * Swallowing lets us observe what happens next instead of dying immediately. */
static int raise_hook(int sig) {
    write(2, "RAISE_HOOK\n", 11);  /* async-signal-safe confirm */
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "raise(%d) intercepted from game (caller=%p):\n",
            sig, __builtin_return_address(0));
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  bt[%d] %s\n", i, syms ? syms[i] : "?");
    free(syms);
    fflush(stderr);
    return 0;  /* swallow — do not deliver signal */
}

/* ── __gnu_Unwind_Find_exidx (ARM EHABI — no C++ exceptions needed) ──────── */
static void *__gnu_Unwind_Find_exidx_stub(void *pc, int *pcount) {
    (void)pc;
    if (pcount) *pcount = 0;
    return NULL;
}

/* ── Bionic-compatible _ctype_ table ─────────────────────────────────────── */
/* Bionic bit flags: _U=0x01 _L=0x02 _D=0x04 _C=0x08 _P=0x10 _S=0x20 _X=0x40 _B=0x80 */
static const char android_ctype_table[257] = {
    0,                                                /* [0]   EOF */
    0x08,                                             /* [1]   0x00 NUL   ctrl */
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,         /* [2-9] 0x01-0x08  ctrl */
    0x28,0x28,0x28,0x28,0x28,                         /* [10-14] 0x09-0x0D HT/LF/VT/FF/CR ctrl+space */
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,         /* [15-22] 0x0E-0x15 ctrl */
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08, /* [23-32] 0x16-0x1F ctrl */
    0xa0,                                             /* [33]  0x20 SP    space+blank */
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10, /* ! " # ... / */
    0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44, /* 0-9 digit+hex */
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,              /* : ; < = > ? @ */
    0x41,0x41,0x41,0x41,0x41,0x41,                   /* A-F upper+hex */
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, /* G-Z upper */
    0x10,0x10,0x10,0x10,0x10,0x10,                   /* [ \ ] ^ _ ` */
    0x42,0x42,0x42,0x42,0x42,0x42,                   /* a-f lower+hex */
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02, /* g-z lower */
    0x10,0x10,0x10,0x10,0x08,                        /* { | } ~ DEL */
    /* 0x80-0xFF: non-ASCII */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
/* _ctype_ is a char* pointing to android_ctype_table[1] so that ptr[c] works for c=0..255 */
static const char *ctype_ptr_val = android_ctype_table + 1;

/* resolve_stream: bionic __sF[n] lands inside sF_fake[] — map back to glibc streams. */
static FILE *resolve_stream(FILE *s) {
    ptrdiff_t off = (char *)s - sF_fake;
    if (off >= 0 && off < (ptrdiff_t)sizeof(sF_fake)) {
        int idx = (int)(off / BIONIC_FILE_SIZE);
        if (idx == 0) return stdin;
        if (idx == 1) return stdout;
        if (idx == 2) return stderr;
    }
    return s;
}

static int    fclose_fake(FILE *s)                               { return fclose(resolve_stream(s)); }
static int    feof_fake(FILE *s)                                 { return feof(resolve_stream(s)); }
static int    fflush_fake(FILE *s)                               { return fflush(resolve_stream(s)); }
static int    fgetc_fake(FILE *s)                                { return fgetc(resolve_stream(s)); }
static char  *fgets_fake(char *b, int n, FILE *s)                { return fgets(b, n, resolve_stream(s)); }
static int    fprintf_fake(FILE *s, const char *fmt, ...)        { va_list ap; va_start(ap, fmt); int r = vfprintf(resolve_stream(s), fmt, ap); va_end(ap); return r; }
static int    fputc_fake(int c, FILE *s)                         { return fputc(c, resolve_stream(s)); }
static int    fputs_fake(const char *b, FILE *s)                 { return fputs(b, resolve_stream(s)); }
static wint_t fputwc_fake(wchar_t c, FILE *s)                   { return fputwc(c, resolve_stream(s)); }
static size_t fread_fake(void *p, size_t sz, size_t n, FILE *s)  { return fread(p, sz, n, resolve_stream(s)); }
static int    fseek_fake(FILE *s, long o, int w)                 { return fseek(resolve_stream(s), o, w); }
static long   ftell_fake(FILE *s)                                { return ftell(resolve_stream(s)); }

/* fwrite_safe: glibc's _IO_fwrite uses NEON/ldm which faults on misaligned src.
 * Copy misaligned source buffers to the heap before passing to fwrite. */
static size_t fwrite_safe(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    stream = resolve_stream(stream);
    /* ptr in loader binary range is always wrong — game heap is at 0xec000000+ */
    if ((uintptr_t)ptr < 0x10000000) {
        fprintf(stderr, "fwrite_safe: suspicious ptr=%p size=%zu nmemb=%zu stream=%p LR=%p\n",
                ptr, size, nmemb, (void *)stream,
                __builtin_return_address(0));
        fflush(stderr);
        return 0;
    }
    if ((uintptr_t)ptr & 7) {
        size_t total = size * nmemb;
        void *buf = malloc(total);
        if (buf) {
            memcpy(buf, ptr, total);
            size_t ret = fwrite(buf, size, nmemb, stream);
            free(buf);
            return ret;
        }
        return 0;  /* malloc failed, can't safely write unaligned buf */
    }
    return fwrite(ptr, size, nmemb, stream);
}

static FILE *fopen_fake(const char *path, const char *mode);
static int   pthread_kill_fake(pthread_t thread, int sig);
static int   open_fake(const char *path, int flags, ...);

/* ── Softfp ABI thunks for math functions ────────────────────────────────── *
 * libgwnext.so (Android armeabi-v7a) uses soft-float calling convention:        *
 * scalars in integer registers r0-r3.  System libm uses hard-float (VFP).    *
 * These thunks (pcs("aapcs")) receive args from int registers and return      *
 * results in int registers, bridging to/from the hard-float system functions. */
static SOFTFP float  acosf_abi(float x)                  { return acosf(x);       }
static SOFTFP float  asinf_abi(float x)                  { return asinf(x);       }
static SOFTFP double atan_abi(double x)                   { return atan(x);        }
static SOFTFP float  atan2f_abi(float y, float x)         { return atan2f(y, x);   }
static SOFTFP float  atanf_abi(float x)                   { return atanf(x);       }
static SOFTFP double atof_abi(const char *s)              { return atof(s);        }
static SOFTFP double cos_abi(double x)                    { return cos(x);         }
static SOFTFP float  cosf_abi(float x)                    { return cosf(x);        }
static SOFTFP double exp_abi(double x)                    { return exp(x);         }
static SOFTFP double exp2_abi(double x)                   { return exp2(x);        }
static SOFTFP float  expf_abi(float x)                    { return expf(x);        }
static SOFTFP double floor_abi(double x)                  { return floor(x);       }
static SOFTFP float  floorf_abi(float x)                  { return floorf(x);      }
static SOFTFP float  log10f_abi(float x)                  { return log10f(x);      }
static SOFTFP float  logf_abi(float x)                    { return logf(x);        }
static SOFTFP double pow_abi(double x, double y)          { return pow(x, y);      }
static SOFTFP float  powf_abi(float x, float y)           { return powf(x, y);     }
static SOFTFP double sin_abi(double x)                    { return sin(x);         }
static SOFTFP float  sinf_abi(float x)                    { return sinf(x);        }
static SOFTFP double tan_abi(double x)                    { return tan(x);         }
static SOFTFP float  strtof_abi(const char *s, char **e)  { return strtof(s, e); }

/* ── Extra math softfp thunks (Geometry Wars 3 calls these with soft-float
 *    convention; route to the hard-float host libm). ───────────────────────── */
static SOFTFP double fmod_abi(double x, double y)            { return fmod(x, y); }
static SOFTFP float  fmodf_abi(float x, float y)            { return fmodf(x, y); }
static SOFTFP float  tanf_abi(float x)                      { return tanf(x); }
static SOFTFP double cosh_abi(double x)                     { return cosh(x); }
static SOFTFP double sinh_abi(double x)                     { return sinh(x); }
static SOFTFP double tanh_abi(double x)                     { return tanh(x); }
static SOFTFP double ceil_abi(double x)                     { return ceil(x); }
static SOFTFP double ldexp_abi(double x, int e)             { return ldexp(x, e); }
static SOFTFP double frexp_abi(double x, int *e)            { return frexp(x, e); }
static SOFTFP double modf_abi(double x, double *i)          { return modf(x, i); }
static SOFTFP float  modff_abi(float x, float *i)           { return modff(x, i); }

/* ── Symbol table ────────────────────────────────────────────────────────── */

static so_default_dynlib default_dynlib[] = {
    /* ── AEABI helpers ──────────────────────────────────────────────────── */
    /* memclr(dst,n) and memset(dst,n,c) have different arg order than glibc */
    { "__aeabi_memclr",   (uintptr_t)__aeabi_memclr_impl },
    { "__aeabi_memclr4",  (uintptr_t)__aeabi_memclr_impl },
    { "__aeabi_memclr8",  (uintptr_t)__aeabi_memclr_impl },
    { "__aeabi_memcpy",   (uintptr_t)memcpy              },
    { "__aeabi_memcpy4",  (uintptr_t)memcpy              },
    { "__aeabi_memcpy8",  (uintptr_t)memcpy              },
    { "__aeabi_memmove",  (uintptr_t)memmove             },
    { "__aeabi_memmove4", (uintptr_t)memmove             },
    { "__aeabi_memmove8", (uintptr_t)memmove             },
    { "__aeabi_memset",   (uintptr_t)__aeabi_memset_impl },
    { "__aeabi_memset4",  (uintptr_t)__aeabi_memset_impl },
    { "__aeabi_memset8",  (uintptr_t)__aeabi_memset_impl },

    /* ── Android-specific (android/log) ────────────────────────────────── */
    { "__android_log_print", (uintptr_t)__android_log_print },
    /* AAssetManager_* live in the later AAssetManager (NDK) block, implemented
     * in aasset_patch.c (filesystem-backed shim). */

    /* ── Standard C / POSIX (forward to glibc) ───────────────────────── */
    { "abort",        (uintptr_t)abort_hook   },
    { "acosf",        (uintptr_t)acosf_abi     },
    { "asinf",        (uintptr_t)asinf_abi    },
    { "atan",         (uintptr_t)atan_abi     },
    { "atan2f",       (uintptr_t)atan2f_abi   },
    { "atanf",        (uintptr_t)atanf_abi    },
    { "atof",         (uintptr_t)atof_abi     },
    { "atoi",         (uintptr_t)atoi         },
    { "calloc",       (uintptr_t)calloc       },
    { "clock_gettime",(uintptr_t)clock_gettime },  /* our safe syscall version */
    { "close",        (uintptr_t)close        },
    { "closedir",     (uintptr_t)closedir     },
    { "cos",          (uintptr_t)cos_abi       },
    { "cosf",         (uintptr_t)cosf_abi     },
    { "dladdr",       (uintptr_t)dladdr       },
    { "exp",          (uintptr_t)exp_abi       },
    { "exp2",         (uintptr_t)exp2_abi     },
    { "expf",         (uintptr_t)expf_abi     },
    { "fclose",       (uintptr_t)fclose_fake   },
    { "feof",         (uintptr_t)feof_fake     },
    { "fegetround",   (uintptr_t)fegetround    },
    { "fesetround",   (uintptr_t)fesetround    },
    { "fflush",       (uintptr_t)fflush_fake   },
    { "fgetc",        (uintptr_t)fgetc_fake    },
    { "fgets",        (uintptr_t)fgets_fake    },
    { "floor",        (uintptr_t)floor_abi      },
    { "floorf",       (uintptr_t)floorf_abi    },
    { "fopen",        (uintptr_t)fopen_fake    },
    { "fprintf",      (uintptr_t)fprintf_fake  },
    { "fputc",        (uintptr_t)fputc_fake    },
    { "fputs",        (uintptr_t)fputs_fake    },
    { "fputwc",       (uintptr_t)fputwc_fake   },
    { "fread",        (uintptr_t)fread_fake    },
    { "free",         (uintptr_t)free          },
    { "fseek",        (uintptr_t)fseek_fake    },
    { "ftell",        (uintptr_t)ftell_fake    },
    { "fwrite",       (uintptr_t)fwrite_safe   },
    { "getenv",       (uintptr_t)getenv       },
    { "gettid",       (uintptr_t)_gettid      },
    { "gettimeofday", (uintptr_t)gettimeofday_safe },
    { "gmtime",       (uintptr_t)gmtime       },
    { "gzclose",      (uintptr_t)gzclose      },
    { "gzgets",       (uintptr_t)gzgets       },
    { "gzopen",       (uintptr_t)gzopen       },
    { "isspace",      (uintptr_t)isspace      },
    { "localtime",    (uintptr_t)localtime    },
    { "localtime_r",  (uintptr_t)localtime_r  },
    { "log",          (uintptr_t)log          },
    { "log10f",       (uintptr_t)log10f_abi    },
    { "logf",         (uintptr_t)logf_abi     },
    { "longjmp",      (uintptr_t)bionic_longjmp },
    { "lseek",        (uintptr_t)lseek        },
    { "malloc",       (uintptr_t)malloc_debug  },
    { "memchr",       (uintptr_t)memchr       },
    { "memcmp",       (uintptr_t)memcmp       },
    { "mkdir",        (uintptr_t)mkdir        },
    { "nanosleep",    (uintptr_t)nanosleep    },
    { "open",         (uintptr_t)open_fake    },
    { "opendir",      (uintptr_t)opendir      },
    { "pow",          (uintptr_t)pow_abi       },
    { "powf",         (uintptr_t)powf_abi     },
    { "prctl",        (uintptr_t)ret0         },
    { "pthread_attr_destroy",      (uintptr_t)pthread_attr_destroy_fake         },
    { "pthread_attr_getschedparam",(uintptr_t)pthread_attr_getschedparam_fake   },
    { "pthread_attr_getstack",     (uintptr_t)pthread_attr_getstack_fake        },
    { "pthread_attr_init",         (uintptr_t)pthread_attr_init_fake            },
    { "pthread_attr_setschedparam",(uintptr_t)pthread_attr_setschedparam_fake   },
    { "pthread_attr_setstacksize", (uintptr_t)pthread_attr_setstacksize_fake    },
    { "pthread_cond_broadcast",  (uintptr_t)pthread_cond_broadcast_fake  },
    { "pthread_cond_destroy",    (uintptr_t)pthread_cond_destroy_fake    },
    { "pthread_cond_init",       (uintptr_t)pthread_cond_init_fake       },
    { "pthread_cond_signal",     (uintptr_t)pthread_cond_signal_fake     },
    { "pthread_cond_timedwait",  (uintptr_t)pthread_cond_timedwait_fake  },
    { "pthread_cond_wait",       (uintptr_t)pthread_cond_wait_fake       },
    { "pthread_create",          (uintptr_t)pthread_create_fake          },
    { "pthread_getspecific",     (uintptr_t)ret0                         },
    { "pthread_join",            (uintptr_t)pthread_join                 },
    { "pthread_kill",            (uintptr_t)pthread_kill_fake            },
    { "pthread_key_create",      (uintptr_t)ret0                         },
    { "pthread_key_delete",      (uintptr_t)ret0                         },
    { "pthread_mutexattr_destroy",(uintptr_t)ret0                        },
    { "pthread_mutexattr_init",  (uintptr_t)ret0                         },
    { "pthread_mutexattr_settype",(uintptr_t)ret0                        },
    { "pthread_mutex_destroy",   (uintptr_t)pthread_mutex_destroy_fake   },
    { "pthread_mutex_init",      (uintptr_t)pthread_mutex_init_fake      },
    { "pthread_mutex_lock",      (uintptr_t)pthread_mutex_lock_fake      },
    { "pthread_mutex_unlock",    (uintptr_t)pthread_mutex_unlock_fake    },
    { "pthread_once",            (uintptr_t)pthread_once                 },
    { "pthread_self",            (uintptr_t)pthread_self                 },
    { "pthread_setname_np",      (uintptr_t)pthread_setname_np           },
    /* The engine sets Android thread priorities that fall outside glibc's
     * SCHED_FIFO range, which trips __pthread_tpp_change_priority's assert
     * (tpp.c:82) and abort()s. Ignore all thread-scheduling requests — default
     * priority is fine on a handheld. */
    { "pthread_setschedparam",       (uintptr_t)ret0 },
    { "pthread_setschedprio",        (uintptr_t)ret0 },
    { "pthread_attr_setschedpolicy", (uintptr_t)ret0 },
    { "pthread_attr_setinheritsched",(uintptr_t)ret0 },
    { "pthread_mutex_setprioceiling",(uintptr_t)ret0 },
    { "sched_setscheduler",          (uintptr_t)ret0 },
    { "sched_setparam",              (uintptr_t)ret0 },
    { "setpriority",                 (uintptr_t)ret0 },
    { "pthread_setspecific",     (uintptr_t)ret0                         },
    { "putchar",      (uintptr_t)putchar      },
    { "puts",         (uintptr_t)puts         },
    { "qsort",        (uintptr_t)qsort        },
    { "raise",        (uintptr_t)raise_hook   },
    { "rand",         (uintptr_t)rand         },
    { "read",         (uintptr_t)read         },
    { "readdir",      (uintptr_t)readdir      },
    { "realloc",      (uintptr_t)realloc      },
    { "sched_get_priority_max", (uintptr_t)sched_get_priority_max },
    { "sched_get_priority_min", (uintptr_t)sched_get_priority_min },
    { "sched_yield",  (uintptr_t)sched_yield  },
    { "sem_destroy",  (uintptr_t)sem_destroy_fake  },
    { "sem_getvalue", (uintptr_t)sem_getvalue_fake },
    { "sem_init",     (uintptr_t)sem_init_fake     },
    { "sem_post",     (uintptr_t)sem_post_fake     },
    { "sem_trywait",  (uintptr_t)sem_trywait_fake  },
    { "sem_wait",     (uintptr_t)sem_wait_fake     },
    { "setjmp",       (uintptr_t)bionic_setjmp  },
    { "_setjmp",      (uintptr_t)bionic_setjmp  },
    { "sigsetjmp",    (uintptr_t)bionic_setjmp  },
    { "siglongjmp",   (uintptr_t)bionic_longjmp },
    { "_longjmp",     (uintptr_t)bionic_longjmp },
    { "fstat",        (uintptr_t)fstat        },
    { "fstat64",      (uintptr_t)fstat64      },
    { "sigaction",    (uintptr_t)ret0         },
    { "sigemptyset",  (uintptr_t)ret0         },
    { "sin",          (uintptr_t)sin_abi       },
    { "sinf",         (uintptr_t)sinf_abi     },
    { "srand",        (uintptr_t)srand        },
    { "stat",         (uintptr_t)stat_hook    },
    { "strcasecmp",   (uintptr_t)strcasecmp   },
    { "strcat",       (uintptr_t)strcat       },
    { "strchr",       (uintptr_t)strchr       },
    { "strcmp",       (uintptr_t)strcmp       },
    { "strcpy",       (uintptr_t)strcpy       },
    { "strerror",     (uintptr_t)strerror     },
    { "strlen",       (uintptr_t)strlen       },
    { "strncasecmp",  (uintptr_t)strncasecmp  },
    { "strncmp",      (uintptr_t)strncmp      },
    { "strncpy",      (uintptr_t)strncpy      },
    { "strpbrk",      (uintptr_t)strpbrk      },
    { "strstr",       (uintptr_t)strstr       },
    { "strtof",       (uintptr_t)strtof_abi   },
    { "strtol",       (uintptr_t)strtol       },
    { "strtoul",      (uintptr_t)strtoul      },
    { "syscall",      (uintptr_t)syscall      },
    { "sysconf",      (uintptr_t)sysconf      },
    { "tan",          (uintptr_t)tan_abi       },
    { "time",         (uintptr_t)time         },
    { "toupper",      (uintptr_t)toupper      },
    { "usleep",       (uintptr_t)usleep       },
    { "vasprintf",    (uintptr_t)vasprintf    },

    /* ── EGL: route eglGetProcAddress to our discovery override, which logs
       every GL/EGL proc the engine requests and returns the real implementation
       (so the engine actually receives valid function pointers instead of NULL).
       eglGetDisplay / eglQueryString are left out of the table on purpose so
       they resolve to the real Mali EGL via the RTLD_DEFAULT fallback. ── */
    { "eglGetProcAddress",(uintptr_t)eglGetProcAddress_ovr },

    /* ── OpenGL ES 2: all symbols resolved so GOT is never left at PLT garbage ── */
    { "glActiveTexture",              (uintptr_t)glActiveTexture               },
    { "glBufferData",                 (uintptr_t)glBufferData                  },
    { "glBufferSubData",              (uintptr_t)glBufferSubData               },
    { "glDeleteBuffers",              (uintptr_t)glDeleteBuffers               },
    { "glGenBuffers",                 (uintptr_t)glGenBuffers                  },
    { "glGetFloatv",                  (uintptr_t)glGetFloatv                   },
    { "glGetIntegerv",                (uintptr_t)glGetIntegerv                 },
    { "glStencilFunc",                (uintptr_t)glStencilFunc                 },
    { "glStencilMask",                (uintptr_t)glStencilMask                 },
    { "glStencilOp",                  (uintptr_t)glStencilOp                   },
    { "glTexSubImage2D",              (uintptr_t)glTexSubImage2DHook           },
    { "glBlendEquationSeparate",      (uintptr_t)glBlendEquationSeparate       },
    { "glBlendFuncSeparate",          (uintptr_t)glBlendFuncSeparate           },
    { "glPixelStorei",                (uintptr_t)glPixelStorei                 },
    { "glReadPixels",                 (uintptr_t)glReadPixels                  },
    { "glIsEnabled",                  (uintptr_t)glIsEnabled                   },
    { "glFrontFace",                  (uintptr_t)glFrontFace                   },
    { "glPolygonOffset",              (uintptr_t)glPolygonOffset_abi           },
    { "glSampleCoverage",             (uintptr_t)glSampleCoverage_abi          },
    { "glUniform1f",                  (uintptr_t)glUniform1f_abi               },
    { "glUniform1fv",                 (uintptr_t)glUniform1fv                  },
    { "glUniform1iv",                 (uintptr_t)glUniform1iv                  },
    { "glUniform2f",                  (uintptr_t)glUniform2f_abi               },
    { "glUniform2fv",                 (uintptr_t)glUniform2fv                  },
    { "glUniform2i",                  (uintptr_t)glUniform2i                   },
    { "glUniform2iv",                 (uintptr_t)glUniform2iv                  },
    { "glUniform3f",                  (uintptr_t)glUniform3f_abi               },
    { "glUniform3iv",                 (uintptr_t)glUniform3iv                  },
    { "glUniform4fv",                 (uintptr_t)glUniform4fvHook              },
    { "glUniform4i",                  (uintptr_t)glUniform4i                   },
    { "glUniform4iv",                 (uintptr_t)glUniform4iv                  },
    { "glUniformMatrix2fv",           (uintptr_t)glUniformMatrix2fv            },
    { "glVertexAttrib1f",             (uintptr_t)glVertexAttrib1f_abi          },
    { "glVertexAttrib4fv",            (uintptr_t)glVertexAttrib4fv             },
    { "glIsBuffer",                   (uintptr_t)glIsBuffer                    },
    { "glIsProgram",                  (uintptr_t)glIsProgram                   },
    { "glIsShader",                   (uintptr_t)glIsShader                    },
    { "glIsTexture",                  (uintptr_t)glIsTexture                   },
    { "glClearDepthf",                (uintptr_t)glClearDepthf_abi             },
    { "glClearStencil",               (uintptr_t)glClearStencil                },
    { "glCopyTexImage2D",             (uintptr_t)glCopyTexImage2D              },
    { "glCopyTexSubImage2D",          (uintptr_t)glCopyTexSubImage2D           },
    { "glDepthRangef",                (uintptr_t)glDepthRangef_abi             },
    { "glDetachShader",               (uintptr_t)glDetachShader                },
    { "glFinish",                     (uintptr_t)glFinish                      },
    { "glFlush",                      (uintptr_t)glFlush                       },
    { "glGenerateMipmap",             (uintptr_t)glGenerateMipmap              },
    { "glGetActiveAttrib",            (uintptr_t)glGetActiveAttrib             },
    { "glGetActiveUniform",           (uintptr_t)glGetActiveUniform            },
    { "glGetAttachedShaders",         (uintptr_t)glGetAttachedShaders          },
    { "glGetBooleanv",                (uintptr_t)glGetBooleanv                 },
    { "glGetBufferParameteriv",       (uintptr_t)glGetBufferParameteriv        },
    { "glGetFramebufferAttachmentParameteriv", (uintptr_t)glGetFramebufferAttachmentParameteriv },
    { "glGetRenderbufferParameteriv", (uintptr_t)glGetRenderbufferParameteriv  },
    { "glGetTexParameterfv",          (uintptr_t)glGetTexParameterfv           },
    { "glGetTexParameteriv",          (uintptr_t)glGetTexParameteriv           },
    { "glGetUniformfv",               (uintptr_t)glGetUniformfv                },
    { "glGetUniformiv",               (uintptr_t)glGetUniformiv                },
    { "glGetVertexAttribfv",          (uintptr_t)glGetVertexAttribfv           },
    { "glGetVertexAttribiv",          (uintptr_t)glGetVertexAttribiv           },
    { "glGetVertexAttribPointerv",    (uintptr_t)glGetVertexAttribPointerv     },
    { "glHint",                       (uintptr_t)glHint                        },
    { "glLineWidth",                  (uintptr_t)glLineWidth_abi               },
    { "glReleaseShaderCompiler",      (uintptr_t)glReleaseShaderCompiler       },
    { "glShaderBinary",               (uintptr_t)glShaderBinary                },
    { "glTexParameterfv",             (uintptr_t)glTexParameterfv              },
    { "glTexParameteriv",             (uintptr_t)glTexParameteriv              },
    { "glValidateProgram",            (uintptr_t)glValidateProgram             },
    { "glDrawElements",               (uintptr_t)glDrawElementsHook           },
    { "glAttachShader",               (uintptr_t)glAttachShader               },
    { "glBindAttribLocation",         (uintptr_t)glBindAttribLocationHook     },
    { "glBindBuffer",                 (uintptr_t)glBindBuffer                 },
    { "glBindFramebuffer",            (uintptr_t)glBindFramebufferHook        },
    { "glBindRenderbuffer",           (uintptr_t)glBindRenderbuffer           },
    { "glBindTexture",                (uintptr_t)glBindTexture                },
    { "glBlendEquation",              (uintptr_t)glBlendEquation              },
    { "glBlendFunc",                  (uintptr_t)glBlendFuncHook              },
    { "glCheckFramebufferStatus",     (uintptr_t)glCheckFramebufferStatus     },
    { "glClear",                      (uintptr_t)glClear                      },
    { "glClearColor",                 (uintptr_t)glClearColorHook             },
    { "glColorMask",                  (uintptr_t)glColorMask                  },
    { "glCompileShader",              (uintptr_t)glCompileShaderHook          },
    { "glCompressedTexImage2D",       (uintptr_t)glCompressedTexImage2DHook   },
    { "glCreateProgram",              (uintptr_t)glCreateProgram              },
    { "glCreateShader",               (uintptr_t)glCreateShader               },
    { "glCullFace",                   (uintptr_t)glCullFace                   },
    { "glDeleteFramebuffers",         (uintptr_t)glDeleteFramebuffers         },
    { "glDeleteProgram",              (uintptr_t)glDeleteProgram              },
    { "glDeleteRenderbuffers",        (uintptr_t)glDeleteRenderbuffers        },
    { "glDeleteShader",               (uintptr_t)glDeleteShader               },
    { "glDeleteTextures",             (uintptr_t)glDeleteTextures             },
    { "glDepthFunc",                  (uintptr_t)glDepthFunc                  },
    { "glDepthMask",                  (uintptr_t)glDepthMaskHook              },
    { "glDisable",                    (uintptr_t)glDisableHook                },
    { "glDisableVertexAttribArray",   (uintptr_t)glDisableVertexAttribArray   },
    { "glDrawArrays",                 (uintptr_t)glDrawArraysHook             },
    { "glEnable",                     (uintptr_t)glEnableHook                 },
    { "glEnableVertexAttribArray",    (uintptr_t)glEnableVertexAttribArray    },
    { "glFramebufferRenderbuffer",    (uintptr_t)glFramebufferRenderbuffer    },
    { "glFramebufferTexture2D",       (uintptr_t)glFramebufferTexture2DHook   },
    { "glGenFramebuffers",            (uintptr_t)glGenFramebuffers            },
    { "glGenRenderbuffers",           (uintptr_t)glGenRenderbuffers           },
    { "glGenTextures",                (uintptr_t)glGenTextures                },
    { "glGetAttribLocation",          (uintptr_t)glGetAttribLocation          },
    { "glGetError",                   (uintptr_t)glGetError                   },
    { "glGetProgramInfoLog",          (uintptr_t)glGetProgramInfoLog          },
    { "glGetProgramiv",               (uintptr_t)glGetProgramiv               },
    { "glGetShaderInfoLog",           (uintptr_t)glGetShaderInfoLog           },
    { "glGetShaderiv",                (uintptr_t)glGetShaderiv                },
    { "glGetString",                  (uintptr_t)glGetString                  },
    { "glGetUniformLocation",         (uintptr_t)glGetUniformLocation         },
    { "glLinkProgram",                (uintptr_t)glLinkProgramHook            },
    { "glRenderbufferStorage",        (uintptr_t)glRenderbufferStorage        },
    { "glScissor",                    (uintptr_t)glScissor                    },
    { "glShaderSource",               (uintptr_t)glShaderSourceHook           },
    { "glTexImage2D",                 (uintptr_t)glTexImage2DHook             },
    { "glTexParameterf",              (uintptr_t)glTexParameterf_abi          },
    { "glTexParameteri",              (uintptr_t)glTexParameteri              },
    { "glUniform1i",                  (uintptr_t)glUniform1i                  },
    { "glUniform3fv",                 (uintptr_t)glUniform3fvHook             },
    { "glUniform4f",                  (uintptr_t)glUniform4fHook              },
    { "glUniformMatrix3fv",           (uintptr_t)glUniformMatrix3fv           },
    { "glUniformMatrix4fv",           (uintptr_t)glUniformMatrix4fvHook       },
    { "glUseProgram",                 (uintptr_t)glUseProgramHook             },
    { "glVertexAttrib2f",             (uintptr_t)glVertexAttrib2f_abi         },
    { "glVertexAttrib3f",             (uintptr_t)glVertexAttrib3f_abi         },
    { "glVertexAttrib4f",             (uintptr_t)glVertexAttrib4f_abi         },
    { "glVertexAttribPointer",        (uintptr_t)glVertexAttribPointerHook    },
    { "glViewport",                   (uintptr_t)glViewport                   },

    /* ── Misc ABI ─────────────────────────────────────────────────────── */
    { "__assert2",            (uintptr_t)__assert2_impl                    },
    { "__cxa_atexit",         (uintptr_t)__cxa_atexit                      },
    { "__cxa_finalize",       (uintptr_t)__cxa_finalize                    },
    { "__gnu_Unwind_Find_exidx", (uintptr_t)__gnu_Unwind_Find_exidx_stub   },
    { "__stack_chk_fail",     (uintptr_t)abort                             },
    { "__stack_chk_guard",    (uintptr_t)&stack_chk_guard_fake             },
    { "__errno",              (uintptr_t)__errno_location                  },
    { "__sF",                 (uintptr_t)sF_fake                           },
    { "stderr",               (uintptr_t)&stderr_fake                      },
    { "_ctype_",              (uintptr_t)&ctype_ptr_val                    },
    { "_tolower_tab_",        (uintptr_t)&tolower_tab_ptr                  },
    { "__isfinite",           (uintptr_t)_isfinite                         },
    { "__signbit",            (uintptr_t)_signbit                          },
    { "__fpclassifyd",        (uintptr_t)__fpclassify                      },
    { "pthread_attr_getschedparam",  (uintptr_t)pthread_attr_getschedparam_fake },
    { "pthread_attr_getstacksize",   (uintptr_t)pthread_attr_getstacksize_fake  },
    { "pthread_attr_setschedparam",  (uintptr_t)pthread_attr_setschedparam_fake },
    { "sched_get_priority_min",      (uintptr_t)sched_get_priority_min          },

    /* ── AAssetManager (NDK) — filesystem-backed shim (aasset_patch.c) ────── */
    { "AAssetManager_fromJava",        (uintptr_t)AAssetManager_fromJava        },
    { "AAssetManager_open",           (uintptr_t)AAssetManager_open           },
    { "AAsset_close",                 (uintptr_t)AAsset_close                 },
    { "AAsset_read",                  (uintptr_t)AAsset_read                  },
    { "AAsset_getLength",             (uintptr_t)AAsset_getLength             },
    { "AAsset_getRemainingLength",    (uintptr_t)AAsset_getRemainingLength    },
    { "AAsset_seek",                  (uintptr_t)AAsset_seek                  },

    /* ── Extra GLES2 functions used by GW3 (real libGLESv2, hard-float).
     *    Only glBlendColor takes float scalars, so it uses the softfp thunk. ─ */
    { "glBlendColor",                 (uintptr_t)glBlendColor_abi             },
    /* eglGetProcAddress override: discover the GL surface the engine requests */
    { "eglGetProcAddress",            (uintptr_t)eglGetProcAddress_ovr       },
    { "glCompressedTexSubImage2D",    (uintptr_t)glCompressedTexSubImage2D    },
    { "glGetShaderPrecisionFormat",   (uintptr_t)glGetShaderPrecisionFormat   },
    { "glGetShaderSource",            (uintptr_t)glGetShaderSource            },
    { "glIsFramebuffer",              (uintptr_t)glIsFramebuffer              },
    { "glIsRenderbuffer",             (uintptr_t)glIsRenderbuffer             },
    { "glStencilFuncSeparate",        (uintptr_t)glStencilFuncSeparate        },
    { "glStencilMaskSeparate",        (uintptr_t)glStencilMaskSeparate        },
    { "glStencilOpSeparate",          (uintptr_t)glStencilOpSeparate          },
    { "glUniform3i",                  (uintptr_t)glUniform3i                  },
    { "glVertexAttrib1fv",            (uintptr_t)glVertexAttrib1fv            },
    { "glVertexAttrib2fv",            (uintptr_t)glVertexAttrib2fv            },
    { "glVertexAttrib3fv",            (uintptr_t)glVertexAttrib3fv            },

    /* ── Float-parameter GL entry points: engine imports these DIRECTLY and
     * calls them soft-float; the real Mali procs are hard-float, so the float
     * args land in the wrong registers (garbage clear colour / uniforms → a
     * black screen). Route them through the SOFTFP thunks (opengl_patch.c). */
    { "glClearColor",     (uintptr_t)glClearColorHook   },
    { "glBlendColor",     (uintptr_t)glBlendColor_abi    },
    { "glClearDepthf",    (uintptr_t)glClearDepthf_abi   },
    { "glDepthRangef",    (uintptr_t)glDepthRangef_abi   },
    { "glLineWidth",      (uintptr_t)glLineWidth_abi     },
    { "glPolygonOffset",  (uintptr_t)glPolygonOffset_abi },
    { "glSampleCoverage", (uintptr_t)glSampleCoverage_abi },
    { "glTexParameterf",  (uintptr_t)glTexParameterf_abi },
    { "glUniform1f",      (uintptr_t)glUniform1f_abi     },
    { "glUniform2f",      (uintptr_t)glUniform2f_abi     },
    { "glUniform3f",      (uintptr_t)glUniform3f_abi     },
    { "glUniform4f",      (uintptr_t)glUniform4fHook     },
    { "glVertexAttrib1f", (uintptr_t)glVertexAttrib1f_abi },
    { "glVertexAttrib2f", (uintptr_t)glVertexAttrib2f_abi },
    { "glVertexAttrib3f", (uintptr_t)glVertexAttrib3f_abi },
    { "glVertexAttrib4f", (uintptr_t)glVertexAttrib4f_abi },

    /* ── Math softfp thunks (GW3 calls these soft-float) ─────────────────── */
    { "fmod",    (uintptr_t)fmod_abi    },
    { "fmodf",   (uintptr_t)fmodf_abi   },
    { "tanf",    (uintptr_t)tanf_abi    },
    { "cosh",    (uintptr_t)cosh_abi    },
    { "sinh",    (uintptr_t)sinh_abi    },
    { "tanh",    (uintptr_t)tanh_abi    },
    { "ceil",    (uintptr_t)ceil_abi    },
    { "ldexp",   (uintptr_t)ldexp_abi   },
    { "frexp",   (uintptr_t)frexp_abi   },
    { "modf",    (uintptr_t)modf_abi    },
    { "modff",   (uintptr_t)modff_abi   },
    { "__gnu_ldivmod_helper",  (uintptr_t)__gnu_ldivmod_helper  },
    { "__gnu_uldivmod_helper", (uintptr_t)__gnu_uldivmod_helper },

    /* ── FMOD: real libfmodex.so / libfmodevent.so are loaded by fmod_preload()
     * and libgwnext's 35 FMOD imports bind to them via so_resolve_link (they
     * are NOT listed here — a dynlib entry would shadow the real symbol).
     * These are the low-level deps the FMOD .so files themselves pull in that
     * glibc doesn't provide, plus the dlopen() shim that swaps FMOD's OpenSL
     * ES backend for the SDL2 one in fmod_opensl.c. */
    { "dlopen",              (uintptr_t)fmod_dlopen              },
    { "dlsym",               (uintptr_t)fmod_dlsym               },
    { "dlclose",             (uintptr_t)fmod_dlclose             },
    { "__android_log_write", (uintptr_t)__android_log_write_shim },
    { "__errno",             (uintptr_t)__errno_shim             },
};

/* ── NVThreadSpawnProc replacement ──────────────────────────────────────────
 * NVThreadSpawnInfo layout (bionic side, all 4-byte words unless noted):
 *   +0  thread_arg  – first arg to pass to the actual thread function
 *   +4  thread_func – the real thread start routine
 *   +8  attach_env  – byte: 1 = need NV JNI context (we skip); 0 = plain call
 * We skip the NV JNI context setup (which needs a live C++ NVThread object)
 * and just call thread_func(thread_arg) directly, matching the cbz path.
 */
static void *nvthread_spawn_proc_hook(void *arg) {
    void  *thread_arg  = *(void **)  ((char *)arg + 0);
    void *(*thread_func)(void *) =
        (void *(*)(void *))(uintptr_t)(*(uint32_t *)((char *)arg + 4));
    free(arg);
    return thread_func(thread_arg);
}

/* ── pthread_kill interceptor ───────────────────────────────────────────────
 * The game stores bionic pthread_t values (small integers or bionic struct
 * pointers) and passes them to pthread_kill.  Glibc's pthread_kill
 * dereferences the handle as a struct pthread* — if the handle is not a real
 * glibc struct, this crashes or sends a signal to the wrong thread.
 * Two layers of interception:
 *   1. pthread_kill_fake: in the dynlib table for libgwnext.so's GOT (if imported)
 *   2. Global pthread_kill: symbol interposition catches calls from SDL2/OpenAL */
static int pthread_kill_fake(pthread_t thread, int sig) {
    (void)thread; (void)sig;
    return 0;
}

/* Global interposition: catches pthread_kill from ALL shared libs. */
int pthread_kill(pthread_t thread, int sig) {
    static int (*real_pk)(pthread_t, int) = NULL;
    if (!real_pk) real_pk = dlsym(RTLD_NEXT, "pthread_kill");
    if (sig == SIGSEGV || sig == SIGABRT || sig == SIGILL || sig == SIGBUS)
        return 0;
    return real_pk(thread, sig);
}

/* ── fopen/open wrappers ─────────────────────────────────────────────────── */
static FILE *fopen_fake(const char *path, const char *mode) {
    return fopen(path, mode);
}

static int open_fake(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return open(path, flags, mode);
}

/* ── patch_gw3 ──────────────────────────────────────────────────────────── *
 * Per-game hooking. For GTA CTW these were OS_ThreadLaunch, OS_ScreenGetWidth,
 * ProcessEvents, AND_SystemInitialize and a fixed load_bias thread-launch hook.
 * Geometry Wars 3 has its own symbol set — discover its equivalents by running
 * with the crash/resolve logging and then hook them here.
 *
 * The hooks below are generic and safe to keep (they no-op if the symbol is
 * absent): the GCC-compatible __cxa_guard_* stubs and the JNI-env accessor. */

static void patch_gw3(void) {
    /* Replace bionic libc++ guard functions with GCC-compatible stubs */
    hook_addr(so_symbol(&gw3_mod, "__cxa_guard_acquire"), (uintptr_t)__cxa_guard_acquire_impl);
    hook_addr(so_symbol(&gw3_mod, "__cxa_guard_release"), (uintptr_t)__cxa_guard_release_impl);
    hook_addr(so_symbol(&gw3_mod, "__cxa_guard_abort"),   (uintptr_t)__cxa_guard_abort_impl);

    /* JNI env accessor (present on some engines; harmless if absent) */
    hook_addr(so_symbol(&gw3_mod, "_Z24NVThreadGetCurrentJNIEnvv"),
              (uintptr_t)NVThreadGetCurrentJNIEnv);

    /* Audio: only stub the engine's audio entry points when real FMOD failed
     * to load (fmod_preload). With FMOD present, Audio::Init drives
     * C_AudioSystem::Initialise -> FMOD EventSystem + our SDL2 OpenSL output,
     * and Audio::Update pumps EventSystem::update() each frame. */
    if (!g_fmod_ok) {
        hook_addr(so_symbol(&gw3_mod, "_ZN5Audio4InitEv"),   (uintptr_t)ret0);
        hook_addr(so_symbol(&gw3_mod, "_ZN5Audio6UpdateEv"), (uintptr_t)ret0);
        fprintf(stderr, "patch_gw3: FMOD unavailable -> Audio::Init/Update stubbed (silent)\n");
    } else {
        fprintf(stderr, "patch_gw3: FMOD active -> Audio::Init/Update left live\n");
    }

    /* The 25 Audio::C_AudioSystem command methods (TriggerOneShotSound,
     * BeginMusicCue, ...) at 0x225e04..0x2277bc used to be stubbed out here
     * because the front end calls them with this == NULL on frame 0.  They
     * are now NULL-guarded by name in qol.c, for both game versions. */

    /* GW3 joypad: make the engine believe one physical joypad is connected and
     * is User 0's master controller, then feed it live SDL pad state.
     *   _Z12GetNoJoypadsv                                   -> 1  (pad count)
     *   _ZN12UserControls27GetMasterUserControllerTypeEv    -> 0  (physical joypad)
     * Without these, _ZN11InputDevice6UpdateEv fires the blocking "reconnect
     * controller" popup (text 0x1aa) the instant a game is active -> no input
     * in-game; type 3 would instead drop the game into on-screen-vstick mode
     * (useless with no touchscreen).  GetJoypadButtons/Axis are pinned to our
     * SDL state so the engine's per-frame input processor can't latch a stale
     * array; pump_joy_input() in jni_patch.c also writes g_JoypadStates[0]. */
    hook_addr(so_symbol(&gw3_mod, "_Z12GetNoJoypadsv"), (uintptr_t)GetNoJoypads_hook);
    hook_addr(so_symbol(&gw3_mod, "_ZN12UserControls27GetMasterUserControllerTypeEv"),
              (uintptr_t)GetMasterUserControllerType_hook);
    hook_addr(so_symbol(&gw3_mod, "_Z16GetJoypadButtonsi"), (uintptr_t)GetJoypadButtons_hook);
    hook_addr(so_symbol(&gw3_mod, "_Z13GetJoypadAxisii"), (uintptr_t)GetJoypadAxis_hook);
    fprintf(stderr, "patch_gw3: joypad hooks -> NoJoypads=1 MasterType=0 Buttons/Axis=SDL\n");

    qol_install();
    fprintf(stderr, "patch_gw3: generic hooks installed\n");
}

/* resolve addr via dladdr and write "  TAG: lib + 0xOFFSET  (sym+delta)\n".
 * Also recognises addresses inside libgwnext.so (loaded via our own loader, so
 * dladdr can't see them) and reports their offset from load_bias — that's
 * the offset you can look up with `objdump -d libgwnext.so`. */
static void write_addr(const char *tag, unsigned addr) {
    char buf[512];
    int n;
    /* Check libgwnext.so range first — our loader's mmap isn't visible to dladdr */
    uintptr_t lb  = gw3_mod.load_bias;
    uintptr_t end = gw3_mod.text_base + gw3_mod.text_size;
    if (lb && addr >= lb && (uintptr_t)addr < end) {
        n = snprintf(buf, sizeof(buf), "  %s: %08x libgwnext.so + 0x%x\n",
                     tag, addr, (unsigned)((uintptr_t)addr - lb));
        write(2, buf, n);
        return;
    }
    Dl_info info;
    if (dladdr((void *)(uintptr_t)addr, &info))
        n = snprintf(buf, sizeof(buf), "  %s: %s + 0x%tx  (sym %s+%td)\n",
            tag,
            info.dli_fname,
            (char *)(uintptr_t)addr - (char *)info.dli_fbase,
            info.dli_sname ? info.dli_sname : "?",
            info.dli_sname ? (char *)(uintptr_t)addr - (char *)info.dli_saddr : 0);
    else
        n = snprintf(buf, sizeof(buf), "  %s: %08x <not in any DSO>\n", tag, addr);
    write(2, buf, n);
}

/* ── SIGSEGV handler: print fault PC, LR, all regs, annotated stack ─────── */
static void segv_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *ctx = uc;
    unsigned r0  = ctx->uc_mcontext.arm_r0;
    unsigned r1  = ctx->uc_mcontext.arm_r1;
    unsigned r2  = ctx->uc_mcontext.arm_r2;
    unsigned r3  = ctx->uc_mcontext.arm_r3;
    unsigned r4  = ctx->uc_mcontext.arm_r4;
    unsigned r5  = ctx->uc_mcontext.arm_r5;
    unsigned r6  = ctx->uc_mcontext.arm_r6;
    unsigned r7  = ctx->uc_mcontext.arm_r7;
    unsigned r8  = ctx->uc_mcontext.arm_r8;
    unsigned r9  = ctx->uc_mcontext.arm_r9;
    unsigned r10 = ctx->uc_mcontext.arm_r10;
    unsigned fp  = ctx->uc_mcontext.arm_fp;   /* r11 */
    unsigned ip  = ctx->uc_mcontext.arm_ip;   /* r12 */
    unsigned pc  = ctx->uc_mcontext.arm_pc;
    unsigned lr  = ctx->uc_mcontext.arm_lr;
    unsigned sp  = ctx->uc_mcontext.arm_sp;
    unsigned fa  = (unsigned)(uintptr_t)si->si_addr;
    char buf[512];
    int n;

    n = snprintf(buf, sizeof(buf),
        "\n=== CRASH sig=%d si_code=%d thread=%08x ===\n"
        "  PC=%08x LR=%08x SP=%08x fault=%08x\n"
        "  r0=%08x r1=%08x r2=%08x r3=%08x\n"
        "  r4=%08x r5=%08x r6=%08x r7=%08x\n"
        "  r8=%08x r9=%08x r10=%08x fp=%08x ip=%08x\n",
        sig, si->si_code, (unsigned)(uintptr_t)pthread_self(),
        pc, lr, sp, fa,
        r0, r1, r2, r3,
        r4, r5, r6, r7,
        r8, r9, r10, fp, ip);
    write(2, buf, n);

    write_addr("PC", pc);
    write_addr("LR", lr);

    /* Annotated stack: first 32 words verbatim, then a deep scan (up to 512
     * words) printing only entries that point into libgwnext.so's .text or
     * another mapped DSO — a poor-man's backtrace (libgwnext omits frame
     * pointers, so fp-chain unwinding is not possible). */
    write(2, "  Stack (SP+0 .. SP+31):\n", 25);
    unsigned *spp = (unsigned *)(uintptr_t)sp;
    uintptr_t lb  = gw3_mod.load_bias;
    uintptr_t gwbeg = gw3_mod.text_base;
    uintptr_t end = gw3_mod.text_base + gw3_mod.text_size;
    for (int i = 0; i < 32; i++) {
        unsigned word = spp[i];
        Dl_info info;
        if (lb && word >= lb && (uintptr_t)word < end) {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x  libgwnext.so+0x%x\n",
                         i, word, (unsigned)((uintptr_t)word - lb));
        } else if (dladdr((void *)(uintptr_t)word, &info) && info.dli_fname) {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x  %s+0x%tx\n",
                i, word,
                info.dli_sname ? info.dli_sname : info.dli_fname,
                info.dli_sname
                    ? (char *)(uintptr_t)word - (char *)info.dli_saddr
                    : (char *)(uintptr_t)word - (char *)info.dli_fbase);
        } else {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x\n", i, word);
        }
        write(2, buf, n);
    }

    write(2, "  Stack scan (code pointers, SP+0 .. SP+2047):\n", 46);
    for (int i = 0; i < 512; i++) {
        unsigned word = spp[i];
        if (lb && (uintptr_t)word >= gwbeg && (uintptr_t)word < end) {
            n = snprintf(buf, sizeof(buf), "    [sp+%04d] %08x  libgwnext.so+0x%x\n",
                         i, word, (unsigned)((uintptr_t)word - lb));
            write(2, buf, n);
            continue;
        }
        Dl_info info;
        if ((uintptr_t)word > 0x10000 &&
            dladdr((void *)(uintptr_t)word, &info) && info.dli_fname && info.dli_sname) {
            n = snprintf(buf, sizeof(buf), "    [sp+%04d] %08x  %s (%s+0x%tx)\n",
                i, word, info.dli_fname, info.dli_sname,
                (char *)(uintptr_t)word - (char *)info.dli_saddr);
            write(2, buf, n);
        }
    }

    /* /proc/self/maps for base addresses */
    write(2, "  /proc/self/maps:\n", 19);
    int mfd = open("/proc/self/maps", O_RDONLY);
    if (mfd >= 0) {
        char mbuf[4096];
        ssize_t rd;
        while ((rd = read(mfd, mbuf, sizeof(mbuf))) > 0)
            write(2, mbuf, rd);
        close(mfd);
    }
    write(2, "\n", 1);

    signal(sig, SIG_DFL);
    raise(sig);
}

/* │─ SIGILL emulation for the ARM PMU cycle-counter read ─────────────────── *
 * Android games read the PMU cycle counter as a high-resolution timer via
 *   mrc p15, #0, Rt, c9, c13, #0   (PMCCNTR)
 * which is privileged and faults with SIGILL under Linux user space (the
 * Cortex-A35 traps it).  Emulate it by returning a monotonic microsecond
 * counter so init sequences and timing loops don't die.  Any cp15 read is
 * treated the same way; genuine illegal instructions fall through to the
 * normal crash report. */
static uint64_t g_ill_start_us;

static uint32_t ill_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    if (!g_ill_start_us) g_ill_start_us = us;
    return (uint32_t)(us - g_ill_start_us);
}

static void ill_handler(int sig, siginfo_t *si, void *ucontext) {
    ucontext_t *uc = (ucontext_t *)ucontext;
    mcontext_t *mc = &uc->uc_mcontext;
    unsigned long *r = (unsigned long *)&mc->arm_r0; /* arm_r0..arm_r15 contiguous */
    uint32_t pc = mc->arm_pc;

    if (!(mc->arm_cpsr & (1u << 5))) {                   /* ARM state */
        uint32_t insn = *(volatile uint32_t *)(uintptr_t)pc;
        if ((insn & 0x0F000F00u) == 0x0E000F00u) {       /* MRC/MCR p15 */
            int rt = (insn >> 12) & 0xF;                 /* Rt field is bits[15:12] */
            uint32_t val = ill_now_us();
            if (rt < 15) r[rt] = val;
            mc->arm_pc = pc + 4;
            return;
        }
    } else {                                             /* Thumb state */
        uint16_t half1 = *(volatile uint16_t *)(uintptr_t)pc;
        int len = ((half1 & 0xF800u) >= 0xE800u) ? 4 : 2;
        uint32_t val = ill_now_us();
        for (int i = 0; i < 13; i++) r[i] = val;         /* fill scratch regs */
        mc->arm_pc = pc + len;
        return;
    }

    /* Unhandled illegal instruction: report like a normal crash. */
    segv_handler(sig, si, ucontext);
}

/* ── Conditional libc time-function patching ───────────────────────────────
 * History: one device (RK3566/dArkOS ARM32) shipped a glibc whose
 * __clock_gettime64 / __gettimeofday64 dispatched through a NULL vDSO pointer
 * and jumped to PC=0 (SIGSEGV).  The workaround overwrote the function's
 * prologue with a trampoline into our own direct-syscall implementation.
 *
 * That workaround was UNCONDITIONAL, and on mainstream glibc (AmberELEC
 * RG351MP glibc 2.38, ROCKNIX glibc 2.41 — GitHub issues #1, #3) the native
 * function works perfectly.  Overwriting a healthy prologue then crashes with
 * SIGILL / ILL_ILLOPC at fn+0 (the trampoline bytes get decoded in the wrong
 * instruction set: our Thumb2 bytes over an ARM-mode libc function).
 *
 * Fix: probe the native function once, under a temporary SIGILL/SIGSEGV/SIGBUS
 * guard, BEFORE any threads are spawned.  Patch only if the native call
 * actually faults.  Healthy devices keep their untouched libc; the one broken
 * device still gets the trampoline.  The trampoline encoding is also made
 * ISA-aware (ARM vs Thumb2) as defence-in-depth for the fault branch. */

static sigjmp_buf            probe_jmp;
static volatile sig_atomic_t probe_faulted;

static void probe_fault_handler(int sig) {
    (void)sig;
    probe_faulted = 1;
    siglongjmp(probe_jmp, 1);
}

/* Call fn(a0, a1) under a fault guard.  Returns 1 if it faulted, else 0.
 * MUST run single-threaded (installs process-wide signal handlers). */
static int libc_time_fn_faults(void *fn, long a0, long a1) {
    struct sigaction sa, old_ill, old_segv, old_bus;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = probe_fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL,  &sa, &old_ill);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS,  &sa, &old_bus);

    probe_faulted = 0;
    if (sigsetjmp(probe_jmp, 1) == 0) {
        int (*f)(long, long) = (int (*)(long, long))fn;
        f(a0, a1);
    }

    sigaction(SIGILL,  &old_ill,  NULL);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS,  &old_bus,  NULL);
    return (int)probe_faulted;
}

/* Overwrite the prologue at `sym` with a literal-load branch to `target_fn`.
 * ISA of the branch matches the ISA of `sym` (Thumb bit in the symbol value);
 * `target_fn`'s own Thumb bit is preserved so interworking is correct.       */
static void install_time_trampoline(void *sym, void *target_fn, const char *name) {
    uint8_t  *fn     = (uint8_t *)((uintptr_t)sym & ~1u);   /* strip Thumb bit */
    int       thumb  = (uintptr_t)sym & 1u;                 /* target ISA of sym */
    uintptr_t target = (uintptr_t)target_fn;               /* keep its Thumb bit */

    uint8_t trampoline[8];
    if (thumb) {
        /* Thumb2:  ldr.w pc, [pc, #0]   (T3 literal load into PC) */
        trampoline[0] = 0xDF; trampoline[1] = 0xF8;
        trampoline[2] = 0x00; trampoline[3] = 0xF0;
    } else {
        /* ARM:     ldr pc, [pc, #-4]    (e51ff004) — loads the word that
         * immediately follows; LDR-into-PC interworks on ARMv5T+, so a
         * Thumb target address (bit0=1) switches state correctly. */
        trampoline[0] = 0x04; trampoline[1] = 0xF0;
        trampoline[2] = 0x1F; trampoline[3] = 0xE5;
    }
    trampoline[4] = (uint8_t)(target);
    trampoline[5] = (uint8_t)(target >> 8);
    trampoline[6] = (uint8_t)(target >> 16);
    trampoline[7] = (uint8_t)(target >> 24);

    uintptr_t pgsz = 4096;
    uintptr_t page = (uintptr_t)fn & ~(pgsz - 1u);
    if (mprotect((void *)page, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
        fprintf(stderr, "%s: mprotect failed: %s\n", name, strerror(errno));
        return;
    }
    memcpy(fn, trampoline, 8);
    __builtin___clear_cache((char *)fn, (char *)fn + 8);
    mprotect((void *)page, pgsz, PROT_READ | PROT_EXEC);
    fprintf(stderr, "%s: %s-mode trampoline @ %p -> %p\n",
            name, thumb ? "Thumb" : "ARM", (void *)fn, target_fn);
}

/* Look up `symname` in libc; probe it; patch only if the native call faults. */
static void patch_libc_time_fn(const char *symname, void *target_fn,
                               long a0, long a1, const char *name) {
    void *libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (!libc) {
        fprintf(stderr, "%s: libc.so.6 not found via dlopen\n", name);
        return;
    }
    void *sym = dlsym(libc, symname);
    dlclose(libc);
    if (!sym) {
        fprintf(stderr, "%s: %s not in libc\n", name, symname);
        return;
    }

    if (!libc_time_fn_faults(sym, a0, a1)) {
        fprintf(stderr, "%s: native %s works, leaving libc untouched\n",
                name, symname);
        return;
    }
    fprintf(stderr, "%s: native %s FAULTS, installing trampoline\n",
            name, symname);
    install_time_trampoline(sym, target_fn, name);
}

/* Probe scratch buffers: sized for the largest 64-bit-time struct glibc uses. */
static long probe_buf[4];

static void patch_libc_clock64(void) {
    patch_libc_time_fn("__clock_gettime64", (void *)clock_gettime64_safe,
                       (long)CLOCK_MONOTONIC, (long)probe_buf,
                       "patch_libc_clock64");
}

static void patch_libc_gettimeofday64(void) {
    patch_libc_time_fn("__gettimeofday64", (void *)gettimeofday64_safe,
                       (long)probe_buf, 0, "patch_libc_gettimeofday64");
}

static void patch_libc_gettimeofday(void) {
    patch_libc_time_fn("gettimeofday", (void *)gettimeofday_safe,
                       (long)probe_buf, 0, "patch_libc_gettimeofday");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    patch_libc_clock64();
    patch_libc_gettimeofday64();
    patch_libc_gettimeofday();

    /* Allow runtime data path override via GW3_DIR env var */
    const char *env_dir = getenv("GW3_DIR");
    if (env_dir)
        snprintf(g_data_path, sizeof(g_data_path), "%s", env_dir);

    /* sF_fake is zero-filled BSS; resolve_stream() maps bionic __sF offsets to real streams.
     * stderr_fake is accessed as &stderr_fake by the game's extern FILE* stderr binding. */
    stderr_fake = stderr;

    /* ── SDL2 init ──────────────────────────────────────────────────── */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    g_window = SDL_CreateWindow(
        "Geometry Wars 3: Dimensions",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SCREEN_W, SCREEN_H,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP
    );
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    g_gl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(g_window, g_gl_ctx);
    SDL_GL_SetSwapInterval(1);

    /* Resolve the ACTUAL panel size. FULLSCREEN_DESKTOP means the window is now
     * the panel's native mode regardless of the SCREEN_W/H we asked for. The
     * engine builds its render target + aspect ratio from whatever we report to
     * setAppWindowSize/setDeviceScreenSize (see jni_patch.c), so a wrong value
     * here is what stretches the image and later overspills the screen. */
    {
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(g_window, &dw, &dh);
        if (dw < 320 || dh < 240) SDL_GetWindowSize(g_window, &dw, &dh);
        if (dw < 320 || dh < 240) {
            SDL_DisplayMode dm;
            if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { dw = dm.w; dh = dm.h; }
        }
        if (dw >= 320 && dh >= 240) { g_screen_w = dw; g_screen_h = dh; }
        fprintf(stderr, "display: panel %dx%d (requested %dx%d)\n",
                g_screen_w, g_screen_h, SCREEN_W, SCREEN_H);
    }

    /* Install signal handlers AFTER SDL_Init so we override SDL2's handlers.
     * SDL2 installs its own SIGSEGV handler during SDL_Init which re-raises
     * (via tgkill, giving si_code=-6) and obscures the real fault PC.
     * Installing here gives us the real hardware-fault PC/LR/stack.
     * Also catch SIGABRT (bionic abort path uses SIGABRT, not SIGSEGV).    */
    {
        struct sigaction sa = { .sa_sigaction = segv_handler,
                                .sa_flags = SA_SIGINFO | SA_RESETHAND };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
        sigaction(SIGBUS,  &sa, NULL);

        /* SIGILL is handled separately: it must persist (no SA_RESETHAND)
         * because the engine reads the PMU cycle counter repeatedly. */
        struct sigaction sa_ill = { .sa_sigaction = ill_handler,
                                    .sa_flags = SA_SIGINFO };
        sigemptyset(&sa_ill.sa_mask);
        sigaction(SIGILL, &sa_ill, NULL);
    }

    /* ── Gamepad ────────────────────────────────────────────────────── */
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_gamepad = SDL_GameControllerOpen(i);
            if (g_gamepad) {
                fprintf(stderr, "Gamepad: %s\n", SDL_GameControllerName(g_gamepad));
                break;
            }
        }
    }

    /* ── Load libgwnext.so (the GW3 engine) ─────────────────────────── */
    char so_path[560];
    snprintf(so_path, sizeof(so_path), "%s/libgwnext.so", g_data_path);
    fprintf(stderr, "so_load: %s\n", so_path);
    if (so_load(&gw3_mod, so_path) < 0) {
        fprintf(stderr, "Failed to load %s\n", so_path);
        return 1;
    }
    fprintf(stderr, "so_load OK\n");

    so_relocate(&gw3_mod);
    fprintf(stderr, "so_relocate OK\n");

    /* Load the game's bundled FMOD libs and the AAssetManager shim BEFORE
     * resolving libgwnext, so its FMOD/AAsset imports bind to real code via
     * so_resolve_link (DT_NEEDED soname match) and the dynlib table. */
    aasset_patch_init();
    g_fmod_ok = (fmod_preload(g_data_path, &fm_mod, &fme_mod, default_dynlib,
                              sizeof(default_dynlib)) == 0);

    so_resolve(&gw3_mod, default_dynlib,
               sizeof(default_dynlib), 0);
    fprintf(stderr, "so_resolve OK\n");

    /* FMOD (Android build) probes an OpenSL ES output at runtime via
     * dlopen("libOpenSLES.so"), which does not exist on Linux — its init will
     * fail at the output stage. If the engine hard-aborts on that, either swap in
     * a desktop FMOD armhf build or shim libOpenSLES. See fmod_patch.c. */

    patch_opengl();
    fprintf(stderr, "patch_opengl OK\n");
    patch_gw3();
    fprintf(stderr, "patch_gw3 OK\n");

    /* ── JNI bootstrap (must run BEFORE so_initialize: the engine's global
       constructors call NVThreadGetCurrentJNIEnv during init_array and then
       invoke methods through the returned env, so fake_env must be populated
       first or it dereferences a zeroed vtable). ── */
    fprintf(stderr, "jni_init...\n");
    jni_init();

    so_flush_caches(&gw3_mod);
    fprintf(stderr, "so_initialize...\n");
    so_initialize(&gw3_mod);
    fprintf(stderr, "so_initialize OK\n");

    fprintf(stderr, "jni_load...\n");
    jni_load(); /* never returns — runs the game loop */

    /* Unreachable */
    SDL_GL_DeleteContext(g_gl_ctx);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
