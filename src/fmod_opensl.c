/* fmod_opensl.c -- a minimal fake "libOpenSLES.so" backed by SDL2 audio.
 *
 * The game's FMOD Ex 4.44.50 (Android build) has exactly one real audio
 * backend that works off-Android: the OpenSL ES output. At init it does
 * dlopen("libOpenSLES.so") + dlsym() for one factory function and five
 * interface-ID data symbols, then drives a tiny slice of the OpenSL ES object
 * model (~10 methods). See the RE notes in fmod_patch.c.
 *
 * We intercept that dlopen (main.c's fmod_dlopen) and hand FMOD this shim.
 * FMOD builds a PCM ring buffer and Enqueue()s block-sized slices of it to our
 * "Android simple buffer queue"; when a block finishes it expects its
 * registered callback to fire so it can Enqueue the next one. We wire that to
 * an SDL2 audio device: the SDL callback drains FMOD's enqueued blocks and,
 * each time one is exhausted, calls FMOD's buffer-queue callback so FMOD
 * refills and re-enqueues. SetPlayState(PLAYING) opens/unpauses the device.
 *
 * Only the surface FMOD actually touches is implemented; every other slot is a
 * success-returning no-op.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <SDL2/SDL.h>

#include "fmod_patch.h"

/* ── OpenSL ES constants (subset) ─────────────────────────────────────────── */
#define SL_RESULT_SUCCESS                        0u
#define SL_BOOLEAN_FALSE                         0u
#define SL_BOOLEAN_TRUE                          1u
#define SL_DATAFORMAT_PCM                        2u
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE  0x800007BDu
#define SL_PLAYSTATE_STOPPED                     1u
#define SL_PLAYSTATE_PAUSED                      2u
#define SL_PLAYSTATE_PLAYING                     3u

typedef uint32_t SLuint32;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef const void *SLInterfaceID;   /* opaque to us — FMOD only round-trips it */

/* An OpenSL "interface" is a pointer to a pointer to a vtable of function
 * pointers; each method takes the interface pointer as its first argument. We
 * model every object/interface as { const void **vt; ...state... } and pass
 * &obj as the interface handle. */

struct sl_object { const void **vt; int kind; };   /* kind: 0 engine, 1 mix, 2 player */
struct sl_itf    { const void **vt; };

/* PCM format FMOD hands us via CreateAudioPlayer's SLDataSource. */
typedef struct {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec;   /* milliHz! e.g. 48000000 == 48 kHz */
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct { SLuint32 locatorType; SLuint32 numBuffers; } SLDataLocator_BufferQueue;
typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct { void *pLocator; void *pFormat; } SLDataSink;

typedef void (*sl_bq_callback)(struct sl_itf *caller, void *pContext);

/* ── shim state ──────────────────────────────────────────────────────────── */
#define QN 64
static struct {
    SDL_AudioDeviceID dev;
    int      freq;          /* Hz */
    int      channels;
    int      opened;

    /* enqueued blocks (pointers into FMOD's own ring buffer) */
    struct { const uint8_t *ptr; SLuint32 size; } q[QN];
    volatile int q_head, q_tail;      /* SPSC-ish; guarded by q_lock */
    SLuint32     q_consumed;          /* bytes taken from q[q_head]   */
    SDL_mutex   *q_lock;

    sl_bq_callback bq_cb;
    void          *bq_ctx;

    /* pending format from CreateAudioPlayer */
    int want_freq, want_channels;

    uint64_t frames_out;             /* stats */
    int      underruns;
} A;

/* ── SDL audio callback: drain FMOD's enqueued PCM ───────────────────────── */
static void sdl_audio_cb(void *ud, Uint8 *out, int outlen)
{
    (void)ud;
    int filled = 0;
    int refills = 0;

    SDL_LockMutex(A.q_lock);
    while (filled < outlen) {
        if (A.q_head == A.q_tail) {
            /* underrun — let FMOD try to enqueue, but bail after a few tries */
            if (A.bq_cb && refills < 4) {
                refills++;
                SDL_UnlockMutex(A.q_lock);
                A.bq_cb((struct sl_itf *)ud, A.bq_ctx);   /* ud == &g_bq_itf */
                SDL_LockMutex(A.q_lock);
                continue;
            }
            memset(out + filled, 0, (size_t)(outlen - filled));
            A.underruns++;
            break;
        }
        const uint8_t *p = A.q[A.q_head].ptr;
        SLuint32 sz = A.q[A.q_head].size;
        SLuint32 avail = sz - A.q_consumed;
        SLuint32 n = (SLuint32)(outlen - filled);
        if (n > avail) n = avail;
        memcpy(out + filled, p + A.q_consumed, n);
        filled += (int)n;
        A.q_consumed += n;
        if (A.q_consumed >= sz) {
            A.q_consumed = 0;
            A.q_head = (A.q_head + 1) % QN;
            if (A.bq_cb) {
                SDL_UnlockMutex(A.q_lock);
                A.bq_cb((struct sl_itf *)ud, A.bq_ctx);   /* FMOD enqueues next */
                SDL_LockMutex(A.q_lock);
            }
        }
    }
    SDL_UnlockMutex(A.q_lock);
    A.frames_out += (uint64_t)(outlen / (A.channels * 2));

    /* Every ~2 s of audio, report the peak sample level so we can tell whether
     * FMOD is mixing real signal (vs. handing us silence). */
    static uint64_t next_report = 0;
    if (A.frames_out >= next_report) {
        next_report = A.frames_out + (uint64_t)(A.freq * 2);
        int peak = 0;
        const int16_t *s = (const int16_t *)out;
        int n = outlen / 2;
        for (int i = 0; i < n; i++) {
            int v = s[i] < 0 ? -s[i] : s[i];
            if (v > peak) peak = v;
        }
        fprintf(stderr, "fmod_opensl: %llu frames out, peak=%d/32767, underruns=%d\n",
                (unsigned long long)A.frames_out, peak, A.underruns);
    }
}

static void open_sdl_device(void)
{
    if (A.opened) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "fmod_opensl: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = A.want_freq   > 0 ? A.want_freq   : 48000;
    want.format   = AUDIO_S16SYS;
    want.channels = A.want_channels > 0 ? (Uint8)A.want_channels : 2;
    want.samples  = 1024;
    want.callback = sdl_audio_cb;
    want.userdata = NULL;   /* set to &g_bq_itf after it's defined; see below */
    extern struct sl_itf g_bq_itf;
    want.userdata = &g_bq_itf;

    A.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!A.dev) {
        fprintf(stderr, "fmod_opensl: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    A.freq     = have.freq;
    A.channels = have.channels;
    A.opened   = 1;
    fprintf(stderr, "fmod_opensl: SDL audio open %d Hz %d ch, %d-sample buffer\n",
            have.freq, have.channels, have.samples);
}

/* ── SLAndroidSimpleBufferQueueItf ───────────────────────────────────────── */
static SLresult bq_Enqueue(struct sl_itf *self, const void *pBuffer, SLuint32 size)
{
    (void)self;
    SDL_LockMutex(A.q_lock);
    int next = (A.q_tail + 1) % QN;
    if (next == A.q_head) {                 /* full — drop oldest */
        A.q_head = (A.q_head + 1) % QN;
        A.q_consumed = 0;
    }
    A.q[A.q_tail].ptr  = (const uint8_t *)pBuffer;
    A.q[A.q_tail].size = size;
    A.q_tail = next;
    SDL_UnlockMutex(A.q_lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(struct sl_itf *self)
{
    (void)self;
    SDL_LockMutex(A.q_lock);
    A.q_head = A.q_tail = 0;
    A.q_consumed = 0;
    SDL_UnlockMutex(A.q_lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(struct sl_itf *self, void *pState)
{
    (void)self;
    if (pState) {
        SDL_LockMutex(A.q_lock);
        SLuint32 count = (SLuint32)((A.q_tail - A.q_head + QN) % QN);
        SDL_UnlockMutex(A.q_lock);
        ((SLuint32 *)pState)[0] = count;   /* count */
        ((SLuint32 *)pState)[1] = 0;       /* index */
    }
    return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(struct sl_itf *self, sl_bq_callback cb, void *ctx)
{
    (void)self;
    A.bq_cb  = cb;
    A.bq_ctx = ctx;
    return SL_RESULT_SUCCESS;
}

/* ── SLPlayItf ───────────────────────────────────────────────────────────── */
static SLresult play_SetPlayState(struct sl_itf *self, SLuint32 state)
{
    (void)self;
    if (state == SL_PLAYSTATE_PLAYING) {
        open_sdl_device();
        if (A.dev) SDL_PauseAudioDevice(A.dev, 0);
        fprintf(stderr, "fmod_opensl: PLAYING\n");
    } else {
        if (A.dev) SDL_PauseAudioDevice(A.dev, 1);
    }
    return SL_RESULT_SUCCESS;
}

/* ── SLAndroidConfigurationItf ───────────────────────────────────────────── */
static SLresult cfg_SetConfiguration(struct sl_itf *self, const char *key,
                                     const void *value, SLuint32 valueSize)
{
    (void)self; (void)key; (void)value; (void)valueSize;
    return SL_RESULT_SUCCESS;
}

/* ── generic no-op slot (returns success) ────────────────────────────────── */
static SLresult sl_noop(void *self, ...) { (void)self; return SL_RESULT_SUCCESS; }

/* ── vtables + interface / object singletons ─────────────────────────────── */
static const void *bq_vt[6];
static const void *play_vt[12];
static const void *cfg_vt[2];
static const void *engine_vt[16];
static const void *obj_vt[10];

struct sl_itf g_bq_itf     = { bq_vt };
static struct sl_itf g_play_itf   = { play_vt };
static struct sl_itf g_cfg_itf    = { cfg_vt };
static struct sl_itf g_engine_itf = { engine_vt };

static struct sl_object g_engine_obj = { obj_vt, 0 };
static struct sl_object g_mix_obj    = { obj_vt, 1 };
static struct sl_object g_player_obj = { obj_vt, 2 };

/* interface-ID sentinels: each points at itself, so whether FMOD passes the
 * dlsym'd address or the value it loaded from it, our compares still match. */
void *SL_IID_ENGINE;
void *SL_IID_PLAY;
void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
void *SL_IID_ANDROIDCONFIGURATION;
void *SL_IID_RECORD;

/* ── SLObjectItf ─────────────────────────────────────────────────────────── */
static SLresult obj_Realize(struct sl_object *self, SLboolean async)
{
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_GetInterface(struct sl_object *self, SLInterfaceID iid, void *pInterface)
{
    struct sl_itf **out = (struct sl_itf **)pInterface;
    if (!out) return SL_RESULT_SUCCESS;

    if (self->kind == 0) {                    /* engine object */
        *out = &g_engine_itf;
    } else if (self->kind == 2) {             /* player object */
        if      (iid == (SLInterfaceID)SL_IID_PLAY)                    *out = &g_play_itf;
        else if (iid == (SLInterfaceID)SL_IID_ANDROIDSIMPLEBUFFERQUEUE) *out = &g_bq_itf;
        else if (iid == (SLInterfaceID)SL_IID_ANDROIDCONFIGURATION)    *out = &g_cfg_itf;
        else                                                          *out = &g_bq_itf;
    } else {                                  /* output-mix object: nothing used */
        *out = &g_engine_itf;
    }
    return SL_RESULT_SUCCESS;
}
static SLresult obj_Destroy(struct sl_object *self)
{
    if (self->kind == 2 && A.dev) {
        SDL_CloseAudioDevice(A.dev);
        A.dev = 0;
        A.opened = 0;
    }
    return SL_RESULT_SUCCESS;
}

/* ── SLEngineItf ─────────────────────────────────────────────────────────── */
static SLresult eng_CreateOutputMix(struct sl_itf *self, struct sl_object **pMix,
                                    SLuint32 numItf, const SLInterfaceID *ids,
                                    const SLboolean *req)
{
    (void)self; (void)numItf; (void)ids; (void)req;
    if (pMix) *pMix = &g_mix_obj;
    return SL_RESULT_SUCCESS;
}
static SLresult eng_CreateAudioPlayer(struct sl_itf *self, struct sl_object **pPlayer,
                                      SLDataSource *src, SLDataSink *snk,
                                      SLuint32 numItf, const SLInterfaceID *ids,
                                      const SLboolean *req)
{
    (void)self; (void)snk; (void)numItf; (void)ids; (void)req;
    if (src && src->pFormat) {
        SLDataFormat_PCM *f = (SLDataFormat_PCM *)src->pFormat;
        if (f->formatType == SL_DATAFORMAT_PCM) {
            A.want_channels = (int)f->numChannels;
            A.want_freq     = (int)(f->samplesPerSec / 1000u);   /* milliHz -> Hz */
        }
    }
    if (src && src->pLocator) {
        SLDataLocator_BufferQueue *l = (SLDataLocator_BufferQueue *)src->pLocator;
        fprintf(stderr, "fmod_opensl: CreateAudioPlayer %d Hz %d ch, %u buffers\n",
                A.want_freq, A.want_channels, l->numBuffers);
    }
    if (pPlayer) *pPlayer = &g_player_obj;
    return SL_RESULT_SUCCESS;
}

/* ── slCreateEngine ─────────────────────────────────────────────────────── */
static SLresult my_slCreateEngine(struct sl_object **pEngine, SLuint32 numOpt,
                                  const void *opts, SLuint32 numItf,
                                  const SLInterfaceID *ids, const SLboolean *req)
{
    (void)numOpt; (void)opts; (void)numItf; (void)ids; (void)req;
    if (pEngine) *pEngine = &g_engine_obj;
    return SL_RESULT_SUCCESS;
}

/* ── one-time table build + dlsym resolver ──────────────────────────────── */
void fmod_opensl_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;

    A.q_lock = SDL_CreateMutex();
    SL_IID_ENGINE                   = &SL_IID_ENGINE;
    SL_IID_PLAY                     = &SL_IID_PLAY;
    SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    SL_IID_ANDROIDCONFIGURATION     = &SL_IID_ANDROIDCONFIGURATION;
    SL_IID_RECORD                   = &SL_IID_RECORD;

    for (size_t i = 0; i < sizeof(obj_vt)/sizeof(*obj_vt); i++)       obj_vt[i]    = (void *)sl_noop;
    for (size_t i = 0; i < sizeof(engine_vt)/sizeof(*engine_vt); i++) engine_vt[i] = (void *)sl_noop;
    for (size_t i = 0; i < sizeof(play_vt)/sizeof(*play_vt); i++)     play_vt[i]   = (void *)sl_noop;
    for (size_t i = 0; i < sizeof(bq_vt)/sizeof(*bq_vt); i++)         bq_vt[i]     = (void *)sl_noop;
    for (size_t i = 0; i < sizeof(cfg_vt)/sizeof(*cfg_vt); i++)       cfg_vt[i]    = (void *)sl_noop;

    obj_vt[0] = (void *)obj_Realize;
    obj_vt[3] = (void *)obj_GetInterface;
    obj_vt[6] = (void *)obj_Destroy;

    engine_vt[2] = (void *)eng_CreateAudioPlayer;
    engine_vt[7] = (void *)eng_CreateOutputMix;

    play_vt[0] = (void *)play_SetPlayState;

    bq_vt[0] = (void *)bq_Enqueue;
    bq_vt[1] = (void *)bq_Clear;
    bq_vt[2] = (void *)bq_GetState;
    bq_vt[3] = (void *)bq_RegisterCallback;
    bq_vt[4] = (void *)bq_Clear;          /* this FMOD build calls Clear at slot 4 */

    cfg_vt[0] = (void *)cfg_SetConfiguration;
}

void *fmod_opensl_sym(const char *name)
{
    if (!name) return NULL;
    if (!strcmp(name, "slCreateEngine"))                 return (void *)my_slCreateEngine;
    if (!strcmp(name, "SL_IID_ENGINE"))                  return &SL_IID_ENGINE;
    if (!strcmp(name, "SL_IID_PLAY"))                    return &SL_IID_PLAY;
    if (!strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))return &SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    if (!strcmp(name, "SL_IID_ANDROIDCONFIGURATION"))    return &SL_IID_ANDROIDCONFIGURATION;
    if (!strcmp(name, "SL_IID_RECORD"))                  return &SL_IID_RECORD;
    if (!strcmp(name, "SL_IID_BUFFERQUEUE"))             return &SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    if (!strcmp(name, "SL_IID_OUTPUTMIX"))               return &SL_IID_ENGINE;
    fprintf(stderr, "fmod_opensl: unhandled dlsym(\"%s\")\n", name);
    return NULL;
}
