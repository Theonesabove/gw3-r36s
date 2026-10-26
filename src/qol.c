/* qol.c -- quality-of-life layer for the R36S port (ported from gw3nx, the
 * Switch port of the same game).  See qol.h for the list.
 *
 * Calling into the engine: libgwnext.so is soft-float (armeabi-v7a), this
 * binary is hard-float.  Every engine function used here takes and returns
 * only integers/pointers, except UISlider's float position, which travels as
 * its raw bits in a core register -- exactly what soft-float does.  So all
 * engine calls below are made through integer-only prototypes. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>

#include "so_util.h"
#include "qol.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"       /* public domain; decodes the game's font textures */
#include "font8x8_basic.h"   /* public domain, github.com/dhepper/font8x8 */

extern so_module gw3_mod;
extern char g_data_path[512];
extern int  g_tv_mode;       /* jni_patch.c: 1.3.0 console UI */

typedef uintptr_t U;
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static U sym(const char *n) {
    U a = so_symbol(&gw3_mod, n);
    if (!a) fprintf(stderr, "qol: %s not found\n", n);
    return a;
}
static uint32_t ticks(void) { return SDL_GetTicks(); }

/* ── hooks with a callable original ───────────────────────────────────────── */

static uint32_t *tramp_alloc(int words) {
    static uint32_t *page;
    static int used = 1024;
    if (used + words > 1024) {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return NULL;
        page = p; used = 0;
    }
    uint32_t *t = page + used;
    used += words;
    return t;
}

/* An ARM instruction that behaves the same at another address: not a branch,
 * not in the unconditional space, no register field naming PC.  (Conservative:
 * an immediate that happens to end in 0xF is refused too.) */
static int insn_movable(uint32_t i) {
    if ((i >> 28) == 0xF) return 0;
    if (((i >> 25) & 7) == 5) return 0;
    if (((i >> 16) & 15) == 15 || ((i >> 12) & 15) == 15 || (i & 15) == 15) return 0;
    return 1;
}

static U make_tramp(U addr, const uint32_t *prefix, int nprefix, const char *name) {
    if (!addr) return 0;
    if (addr & 1) { fprintf(stderr, "hook: %s is Thumb, skipped\n", name); return 0; }
    const uint32_t *p = (const uint32_t *)addr;
    if (!insn_movable(p[0]) || !insn_movable(p[1])) {
        fprintf(stderr, "hook: %s starts with %08x %08x, can't wrap\n", name, p[0], p[1]);
        return 0;
    }
    uint32_t *t = tramp_alloc(nprefix + 4);
    if (!t) return 0;
    for (int i = 0; i < nprefix; i++) t[i] = prefix[i];
    t[nprefix + 0] = p[0];
    t[nprefix + 1] = p[1];
    t[nprefix + 2] = 0xe51ff004;          /* ldr pc, [pc, #-4] */
    t[nprefix + 3] = (uint32_t)(addr + 8);
    __builtin___clear_cache((char *)t, (char *)(t + nprefix + 4));
    return (U)t;
}

U hook_wrap(U addr, U fn, const char *name) {
    U t = make_tramp(addr, NULL, 0, name);
    if (t) hook_arm(addr, fn);
    return t;
}

int hook_null_guard(U addr, const char *name) {
    static const uint32_t guard[] = {
        0xe3500000,   /* cmp   r0, #0  */
        0x03a00000,   /* moveq r0, #0  */
        0x012fff1e,   /* bxeq  lr      */
    };
    U t = make_tramp(addr, guard, 3, name);
    if (t) hook_arm(addr, t);
    return t != 0;
}

/* ── settings (gw3.ini) ───────────────────────────────────────────────────── */

typedef struct { const char *name, *label; float particles, background; } Preset;
/* AUTO = whatever the game picks for this device (the original port's look). */
static const Preset kPresets[] = {
    {"Auto (game default)", "AUTO",    -1.0f,  -1.0f},
    {"Low",                 "LOW",      0.25f,  0.2f},
    {"Android High",        "ANDROID",  0.45f,  0.325f},
    {"Console (PS4/XB1)",   "CONSOLE",  1.0f,   1.0f},
};
#define NPRESETS ((int)(sizeof kPresets / sizeof kPresets[0]))
static const float kDetailMax = 1.0f;   /* slider end = the Console preset */

static struct { float particles, background; int fps, nintendo, large_menus, stage_zoom; } g_set = {-1.0f, -1.0f, 0, 1, 1, 0};
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static void ini_path(char *p, size_t n) { snprintf(p, n, "%s/gw3.ini", g_data_path); }
static void settings_save(void) {
    char p[600]; ini_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "particles=%.3f\nbackground=%.3f\nfps=%d\nnintendo_layout=%d\nlarge_menus=%d\nstage_zoom=%d\n",
            g_set.particles, g_set.background, g_set.fps, g_set.nintendo, g_set.large_menus, g_set.stage_zoom);
    fclose(f);
}
static int preset_match(float p, float b) {
    for (int i = 0; i < NPRESETS; i++)
        if (fabsf(kPresets[i].particles - p) < 0.02f && fabsf(kPresets[i].background - b) < 0.02f) return i;
    return -1;
}
int qol_nintendo_layout(void) { return g_set.nintendo; }

/* ── stage zoom ─────────────────────────────────────────────────────────────
 * The stage camera's field of view is gMapZoom * 0.6283 (36 degrees); the
 * 0.6283 is a literal in the render code, and C_LuaCamera (level intros, boss
 * cut-ins) has its own copy.  Scaling both narrows the view, so the arena and
 * everything in it is drawn bigger; the camera already follows the ship.
 * Takes effect on the next frame. */
static const float kStageZoom[] = {1.0f, 1.1f, 1.2f, 1.3f};
static const char *kStageZoomName[] = {"OFF", "10%", "20%", "30%"};
static float *g_fov_lit[2];       /* stage camera, intro/cut-in camera */
static const float kFovConst = 0.62831855f;

static void stage_zoom_find(void) {
    /* the stage camera: vldr s12, [pc, #0x3f0] (ed9f6afc) loading the literal; 1.3.0, 1.0.0 */
    static const uint32_t kSites[][2] = {{0x41ad38, 0x41b130}, {0x41b860, 0x41bc58}};
    for (int i = 0; i < 2 && !g_fov_lit[0]; i++) {
        uint32_t *ld = (uint32_t *)(gw3_mod.load_bias + kSites[i][0]);
        float *lit = (float *)(gw3_mod.load_bias + kSites[i][1]);
        if ((U)lit < gw3_mod.text_base + gw3_mod.text_size && *ld == 0xed9f6afc && *lit == kFovConst) g_fov_lit[0] = lit;
    }
    U lua = so_symbol(&gw3_mod, "_ZNK11C_LuaCamera12CalculateFovEf");   /* vmov; vldr s15, [pc, #0x18] */
    if (lua && *(float *)(lua + 0x24) == kFovConst) g_fov_lit[1] = (float *)(lua + 0x24);
    for (int i = 0; i < 2; i++)
        if (g_fov_lit[i]) mprotect((void *)((U)g_fov_lit[i] & ~4095u), 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    fprintf(stderr, "qol: stage zoom %s\n", g_fov_lit[0] ? "available" : "not available (other game version)");
}
static void stage_zoom_apply(void) {
    for (int i = 0; i < 2; i++)
        if (g_fov_lit[i]) *g_fov_lit[i] = kFovConst / kStageZoom[g_set.stage_zoom];
}

/* ── achievements ─────────────────────────────────────────────────────────── */

/* In the game's E_Achievement order: the enum's names are in the game's
 * reflection data (Xt::Annotation<E_Achievement>::fields: LevelUpADrone,
 * LevelAllDrones, KillOneHundredEnemiesWithOneBomb, EarnFirstGoldMedal, ...;
 * 30-33 are online-only). Names and Android thresholds from Google Play. */
typedef struct { const char *name, *desc; } Ach;
static const Ach kAch[] = {
    {"Specialist", "Upgrade any one Drone to max level"},
    {"Maximise", "Upgrade all 5 Drones to max level"},
    {"Boom", "Kill 100 enemies with a single Smart Bomb"},
    {"First Step", "Earn 3 stars on any Adventure Mode level"},
    {"Geometry Warrior", "Earn 3 stars on all 50 Adventure Mode levels"},
    {"Keen Adventurer", "Defeat the Topaz boss in Adventure Mode"},
    {"Millionaire", "Score 1 million on any Adventure Mode level"},
    {"Multi Millionaire", "Score 100 million on any Adventure Mode level"},
    {"Geomtastic", "Reach a x1000 multiplier on any Adventure level"},
    {"Kill Them All", "Kill every enemy type at least once"},
    {"Dying Time", "Be killed by every enemy type at least once"},
    {"Powered Up", "Activate each Super State at least once"},
    {"Best Friends", "Defeat the Garnet boss in Co-op Mode"},
    {"BFF", "Earn 3 stars on all 10 Co-op Mode levels"},
    {"Gatherer", "Collect 500,000 Geoms in Adventure Mode"},
    {"Beat Craig", "Beat the dev's 9,451,570 on Peanut Dreams"},
    {"Beat Col", "Beat the dev's 2,154,750 on Gate Dash"},
    {"Beat Phil", "Beat the dev's 21,478,525 on Nufo Flow"},
    {"Rocket Destroyer", "Destroy 10,000 Rocket enemies in total"},
    {"Deadliest", "Complete Deadline Classic without losing a life"},
    {"Bossman", "Beat any boss without a Drone Super or Smart Bomb"},
    {"Retro", "Score 5 million on Evolved Classic before dying"},
    {"Sharp Shooter", "Score 500,000 on Twisted Blister without moving"},
    {"Survivalist", "Survive 30 s on Pacifism Classic without a gate"},
    {"Peacekeeper", "Survive 60 s on Deflector without shooting, pass"},
    {"Stationary", "Score 250,000 on Waves Classic without moving"},
    {"Bullet Proof", "Score 1 million on Maze without dying"},
    {"Beat Andy", "Beat the dev's 8,418,850 on Pacifism Classic"},
    {"King Killer", "Spend 30 s outside the King Zones in King Classic"},
    {"Cleaning Up", "Destroy the cleaner"},
};
#define NACH ((int)(sizeof kAch / sizeof kAch[0]))
static char g_ach_date[NACH][16];   /* "" = locked */
static char g_pop_name[64], g_pop_desc[96];
static uint32_t g_pop_start, g_pop_until;
static int g_pop_pending;            /* an unlock waiting for the render thread's clock */

static void ach_path(char *p, size_t n) { snprintf(p, n, "%s/achievements.txt", g_data_path); }
static int ach_count(void) { int n = 0; for (int i = 0; i < NACH; i++) n += g_ach_date[i][0] != 0; return n; }

static void upper(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }

static void achievement_unlocked(int i) {
    pthread_mutex_lock(&g_mx);
    if (i >= 0 && i < NACH && !g_ach_date[i][0]) {
        time_t t = time(NULL);
        strftime(g_ach_date[i], sizeof g_ach_date[i], "%Y-%m-%d", localtime(&t));
        char p[600]; ach_path(p, sizeof p);
        FILE *f = fopen(p, "a");
        if (f) { fprintf(f, "%d %s %s\n", i, g_ach_date[i], kAch[i].name); fclose(f); }
        snprintf(g_pop_name, sizeof g_pop_name, "%s", kAch[i].name); upper(g_pop_name);
        snprintf(g_pop_desc, sizeof g_pop_desc, "%s", kAch[i].desc);
        g_pop_pending = 1;
        fprintf(stderr, "achievement %d unlocked: %s\n", i, kAch[i].name);
    }
    pthread_mutex_unlock(&g_mx);
}

/* The achievements in the format of the game's credits XML, so the Credits
 * screen lists them: JobTitle (name) on top, Name (description) under it. */
static char g_xml[16384];
static long build_achievements_xml(void) {
    pthread_mutex_lock(&g_mx);
    size_t n = 0;
#define PUT(...) n += (size_t)snprintf(g_xml + n, n < sizeof g_xml ? sizeof g_xml - n : 0, __VA_ARGS__)
    PUT("<credits>\n\t<Company Name = \"%d of %d unlocked\" >\n", ach_count(), NACH);
    for (int pass = 0; pass < 2; pass++) {
        int any = 0;
        for (int i = 0; i < NACH; i++) any |= (g_ach_date[i][0] == 0) == pass;
        if (!any) continue;
        PUT("\t\t<Group Name = \"%s\">\n", pass ? "LOCKED" : "UNLOCKED");
        for (int i = 0; i < NACH; i++) {
            if ((g_ach_date[i][0] == 0) != pass) continue;
            char name[64]; snprintf(name, sizeof name, "%s", kAch[i].name); upper(name);
            /* names/descriptions contain no XML specials ('&', '"', '<', '>') */
            if (pass) PUT("\t\t\t<Person JobTitle = \"%s\" Name = \"%s\" />\n", name, kAch[i].desc);
            else      PUT("\t\t\t<Person JobTitle = \"%s\" Name = \"%s - %s\" />\n", name, kAch[i].desc, g_ach_date[i]);
        }
        PUT("\t\t</Group>\n");
    }
    PUT("\t</Company>\n</credits>\n");
#undef PUT
    pthread_mutex_unlock(&g_mx);
    return n < sizeof g_xml ? (long)n : (long)sizeof g_xml - 1;
}

/* ── engine hooks ─────────────────────────────────────────────────────────── */

static int ret1(void) { return 1; }

/* Services::RegisterAchievementProgress(user, E_Achievement, float progress) */
static void h_ach_progress(int user, int ach, uint32_t progress) {
    (void)user;
    if (bitsf(progress) >= 1.0f) achievement_unlocked(ach);
}

/* Google Play sign-in is gone: hide the "SIGN IN" prompts.
 * LeaderboardSignInNode::Update(float) / AppStoreButton::Update(float) */
static void (*f_set_visible)(U node, int on);
static void h_hide_update(U self, uint32_t dt) { (void)dt; if (f_set_visible) f_set_visible(self, 0); }

/* C_DetailSettings::GetParticleComplexity / GetBackgroundComplexity: the
 * effects detail; AUTO keeps the game's own value. */
static uint32_t (*o_particles)(void), (*o_background)(void);
static uint32_t h_particles(void)  { return g_set.particles  < 0 ? o_particles()  : fbits(g_set.particles); }
static uint32_t h_background(void) { return g_set.background < 0 ? o_background() : fbits(g_set.background); }

static int h_joypad_connected(int pad) { return pad == 0; }

/* ── Options > Graphics (1.3.0 console UI) ───────────────────────────────────
 * The Options list gets a GRAPHICS entry.  Picking it opens the game's Audio
 * page in "graphics mode", built from the same widgets: two sliders (the page
 * only drives its first two slider rows), then PRESET, BUTTONS and FPS rows
 * that cycle with A.  The Achievements tile opens the Credits screen fed with
 * the achievements list.  Offsets are from 1.3.0 build 101. */
static U (*o_setmenu)(U self, U menu);
static U (*o_opt_event)(U self, U node, U ev, U ctx);
static void (*o_aud_init)(U self);
static U (*o_aud_back)(U self);
static U (*o_aud_event)(U self, U node, U ev, U ctx);
static void (*o_title)(U self, U text);
static void (*o_title2)(U self, U text, U transition);
static U (*o_credits_dtor)(U self);
static U (*f_get_label)(U item);
static void (*f_set_text)(U label, U text);
static U (*f_menu_create)(U self);
static void (*f_base_init)(U self);
static void (*f_set_slider)(U slider, uint32_t pos);
static uint32_t (*f_get_slider)(U slider);
static U (*f_front_back)(U self);
static void (*f_enter)(U seq, U id);
static U g_menuseq, vt_options;

static U g_item;            /* our entry in the Options list */
static int g_mode;          /* the next Audio page is the Graphics page */
static U g_page;            /* the live Graphics page */
static U g_sl[2], g_row_preset, g_row_buttons, g_row_fps, g_row_size, g_row_zoom;
static int g_preset;        /* shown preset, -1 = custom */
static float g_auto[2];     /* the game's own values, for AUTO */
static int g_ach_mode;      /* the Credits screen shows the achievements */

static char L_title[] = "GRAPHICS", L_ach[] = "ACHIEVEMENTS";
static char L_part[] = "PARTICLE DETAIL", L_bg[] = "BACKGROUND DETAIL";
static char L_preset[NPRESETS + 1][32];   /* [0] = custom */
static char L_buttons[2][32] = {"BUTTONS: XBOX", "BUTTONS: R36S"};
static char L_fps[2][32] = {"FPS COUNTER: OFF", "FPS COUNTER: ON"};
/* MENU SIZE applies at the next start: [value][differs from what's running] */
static char L_size[2][2][40] = {{"MENU SIZE: NORMAL", "MENU SIZE: NORMAL (RESTART)"},
                                {"MENU SIZE: LARGE", "MENU SIZE: LARGE (RESTART)"}};
static char L_zoom[4][32] = {"STAGE ZOOM: OFF", "STAGE ZOOM: 10%", "STAGE ZOOM: 20%", "STAGE ZOOM: 30%"};
static int g_ui_zoom_possible;   /* 1.3.0 console UI on a screen narrower than 16:9 */
static float g_ui_zoom = 1.0f;   /* the zoom running now */
static U size_label(void) { return (U)L_size[g_set.large_menus][g_set.large_menus != (g_ui_zoom > 1.0f)]; }

#define SLIDER(item) (*(U *)((item) + 0x248))
#define FIELD(obj, off) (*(U *)((obj) + (off)))

static U h_setmenu(U self, U menu) {
    if (FIELD(self, 0) == vt_options) {
        U (*add)(U, U, U) = (void *)FIELD(FIELD(menu, 0), 0x58);
        g_item = add(menu, (U)L_title, 3);
        fprintf(stderr, "qol: Options gets a Graphics entry\n");
    }
    return o_setmenu(self, menu);
}

static U h_opt_event(U self, U node, U ev, U ctx) {
    g_mode = 0;
    if (node && node == g_item) { g_mode = 1; node = FIELD(self, 0x1b0); }   /* the AUDIO entry */
    return o_opt_event(self, node, ev, ctx);
}

static U retitle(U text) {
    if (!text) return text;
    if ((g_mode || g_page) && !strcmp((const char *)text, "AUDIO")) return (U)L_title;
    if (g_ach_mode && !strcmp((const char *)text, "CREDITS")) return (U)L_ach;
    return text;
}
static void h_title(U self, U text) { o_title(self, retitle(text)); }
static void h_title2(U self, U text, U tr) { o_title2(self, retitle(text), tr); }

static void set_label(U item, U text) {
    U label = f_get_label(item);
    if (label) f_set_text(label, text);
}

static void show_preset_values(int i) {
    float p = kPresets[i].particles, b = kPresets[i].background;
    if (p < 0) { p = g_auto[0]; b = g_auto[1]; }
    f_set_slider(SLIDER(g_sl[0]), fbits(fminf(p / kDetailMax, 1.0f)));
    f_set_slider(SLIDER(g_sl[1]), fbits(fminf(b / kDetailMax, 1.0f)));
}

static void h_aud_init(U self) {
    if (!g_mode) { o_aud_init(self); return; }
    g_mode = 0;
    g_page = self;
    f_base_init(self);
    FIELD(self, 0x1ac) = fbits(180.0f);   /* as the Audio page does */
    U menu = f_menu_create(self);
    U (*add_slider)(U, U) = (void *)FIELD(FIELD(menu, 0), 0x88);
    U (*add_item)(U, U, U) = (void *)FIELD(FIELD(menu, 0), 0x58);
    g_sl[0] = add_slider(menu, (U)L_part);
    g_sl[1] = add_slider(menu, (U)L_bg);
    g_auto[0] = bitsf(o_particles ? o_particles() : fbits(0.45f));
    g_auto[1] = bitsf(o_background ? o_background() : fbits(0.325f));
    g_preset = preset_match(g_set.particles, g_set.background);
    g_row_preset = add_item(menu, (U)L_preset[g_preset + 1], 3);
    g_row_buttons = add_item(menu, (U)L_buttons[g_set.nintendo], 3);
    g_row_fps = add_item(menu, (U)L_fps[g_set.fps], 3);
    g_row_size = g_ui_zoom_possible ? add_item(menu, size_label(), 3) : 0;
    g_row_zoom = g_fov_lit[0] ? add_item(menu, (U)L_zoom[g_set.stage_zoom], 3) : 0;
    FIELD(self, 0x1b0) = g_sl[0];         /* the page's own slider fields */
    FIELD(self, 0x1b4) = g_sl[1];
    FIELD(self, 0x198) = 4;
    o_setmenu(self, menu);
    if (g_preset >= 0) show_preset_values(g_preset);
    else {
        f_set_slider(SLIDER(g_sl[0]), fbits(fminf(g_set.particles / kDetailMax, 1.0f)));
        f_set_slider(SLIDER(g_sl[1]), fbits(fminf(g_set.background / kDetailMax, 1.0f)));
    }
    FIELD(self, 0x1c4) = fbits(-1.0f);
    fprintf(stderr, "qol: Graphics page open\n");
}

static U h_aud_event(U self, U node, U ev, U ctx) {
    if (self == g_page && node && (node == g_row_preset || node == g_row_buttons || node == g_row_fps ||
                                       (g_row_size && node == g_row_size) || (g_row_zoom && node == g_row_zoom))) {
        if (node == g_row_preset) {
            g_preset = (g_preset + 1) % NPRESETS;   /* custom -> the first preset */
            show_preset_values(g_preset);
            set_label(g_row_preset, (U)L_preset[g_preset + 1]);
        } else if (node == g_row_buttons) {
            g_set.nintendo = !g_set.nintendo;       /* takes effect at once */
            set_label(g_row_buttons, (U)L_buttons[g_set.nintendo]);
        } else if (node == g_row_zoom) {
            g_set.stage_zoom = (g_set.stage_zoom + 1) % 4;
            stage_zoom_apply();
            set_label(g_row_zoom, (U)L_zoom[g_set.stage_zoom]);
        } else if (node == g_row_size) {
            g_set.large_menus = !g_set.large_menus;
            set_label(g_row_size, size_label());
        } else {
            g_set.fps = !g_set.fps;
            set_label(g_row_fps, (U)L_fps[g_set.fps]);
        }
        return 1;
    }
    return o_aud_event(self, node, ev, ctx);
}

static U h_aud_back(U self) {
    if (self != g_page) return o_aud_back(self);
    g_page = 0;
    float v[2];
    for (int i = 0; i < 2; i++) v[i] = bitsf(f_get_slider(SLIDER(g_sl[i]))) * kDetailMax;
    int is_auto = g_preset == 0 && fabsf(v[0] - fminf(g_auto[0], kDetailMax)) < 0.01f
                                && fabsf(v[1] - fminf(g_auto[1], kDetailMax)) < 0.01f;
    g_set.particles = is_auto ? -1.0f : v[0];
    g_set.background = is_auto ? -1.0f : v[1];
    settings_save();
    fprintf(stderr, "qol: Graphics -> particles %.2f, background %.2f (%s), buttons %s, fps %d, large menus %d, stage zoom %s\n",
            v[0], v[1], is_auto ? "auto" : "set", g_set.nintendo ? "R36S" : "Xbox", g_set.fps, g_set.large_menus,
            kStageZoomName[g_set.stage_zoom]);
    U r = f_front_back(self);
    *(uint8_t *)(self + 0x1c8) = (uint8_t)r;
    return r;
}

static U h_credits_dtor(U self) { g_ach_mode = 0; return o_credits_dtor(self); }

/* CreditsConsoleController::Init -> LoadXML appends the XML's lines to the
 * global creditsVector and never clears it, so a second visit showed the first
 * visit's list.  Empty it (end = begin) first.
 * ponytail: the dropped CreditLines aren't destroyed (a few KB leaked per visit) */
static void (*o_credits_init)(U self);
static U *g_credits_vec;
static void h_credits_init(U self) { if (g_credits_vec) g_credits_vec[1] = g_credits_vec[0]; o_credits_init(self); }

static void port_menu_open_achievements(void);

/* Services::ShowAchievementsUI(int user): the native list on the console UI,
 * the port menu's list otherwise. */
static void h_show_achievements(int user) {
    (void)user;
    if (g_tv_mode && f_enter && g_menuseq) {
        g_ach_mode = 1;
        fprintf(stderr, "qol: Achievements -> credits screen\n");
        f_enter(g_menuseq, 9);   /* the Credits screen */
    } else {
        port_menu_open_achievements();
    }
}

/* ── larger menus on 4:3 (R36S) ────────────────────────────────────────────
 * The console UI is a 640x360-unit canvas seen by a 3D camera (45-degree
 * field of view, CalculateUiTransformMatrix).  GameApp::SetWindowSize keeps
 * all 640 units of width on narrower screens and pads the height, so on the
 * 4:3 panel every menu shrinks to 3/4 and a quarter of the screen is empty.
 * "Large" narrows the camera's field of view by up to 4/3 (fit the height
 * instead) and shrinks the canvas rectangle to what is then visible, so items
 * anchored to the screen edges (title bar, HUD) stay on screen. */
static void (*o_setwin)(U cfg, int w, int h);
static void (*f_safe_region)(U cfg, int w, int h);
static U (*f_get_config)(void);
#define CFGF(cfg, off) (*(float *)((cfg) + (off)))

static void h_setwin(U cfg, int w, int h) {
    o_setwin(cfg, w, h);
    if (g_ui_zoom <= 1.0f || h <= 0) return;
    float aspect = (float)w / h, vw = 640.f / g_ui_zoom, vh = vw / aspect;
    CFGF(cfg, 0x38) = 320 - vw / 2; CFGF(cfg, 0x3c) = 320 + vw / 2;   /* canvas left/right */
    CFGF(cfg, 0x40) = 180 - vh / 2; CFGF(cfg, 0x44) = 180 + vh / 2;   /* top/bottom */
    CFGF(cfg, 0x58) = vw; CFGF(cfg, 0x5c) = vh;
    f_safe_region(cfg, w, h);                                          /* the safe area from the new canvas */
    fprintf(stderr, "qol: larger menus: canvas %.0fx%.0f units (zoom %.2f)\n", vw, vh, g_ui_zoom);
}

/* TitleBar_GetScreenEdgeMargin(): the title bar uses the safe area's corner as a
 * margin from the canvas edges, so take the (now non-zero) canvas left off. */
static void h_title_margin(float *out) {
    U cfg = f_get_config();
    out[0] = CFGF(cfg, 0x48) - CFGF(cfg, 0x38);
    out[1] = CFGF(cfg, 0x4c);
}

static void ui_zoom_install(void) {
    extern int g_screen_w, g_screen_h;
    if (g_screen_h <= 0) return;
    float z = (16.f / 9) / ((float)g_screen_w / g_screen_h);
    if (z > 4.f / 3) z = 4.f / 3;
    if (z < 1.01f) return;                                  /* 16:9 or wider: nothing to gain */
    g_ui_zoom_possible = 1;
    if (!g_set.large_menus) return;
    U setwin = sym("_ZN7GameApp13SetWindowSizeER12C_GameConfigii");
    U margin = sym("_Z28TitleBar_GetScreenEdgeMarginv");
    U ui = sym("_Z26CalculateUiTransformMatrixv");
    f_safe_region = (void *)sym("_ZN7GameApp15SetupSafeRegionER12C_GameConfigii");
    f_get_config = (void *)sym("_ZN7GameApp9GetConfigEv");
    if (!setwin || !margin || !ui || !f_safe_region || !f_get_config) return;
    uint32_t *fov = (uint32_t *)(ui + 0x164);               /* movw r1, #0x0fdb ; movt r1, #0x3f49 = 0.785398 */
    if (fov[0] != 0xe3001fdb || fov[1] != 0xe3431f49 || !make_tramp(setwin, NULL, 0, "SetWindowSize")) {
        fprintf(stderr, "qol: larger menus not available (other game version)\n");
        return;
    }
    o_setwin = (void *)hook_wrap(setwin, (U)h_setwin, "GameApp::SetWindowSize");
    hook_addr(margin, (U)h_title_margin);
    float f = 2 * atanf(tanf(0.785398f / 2) / z);
    uint32_t b = fbits(f);
    mprotect((void *)((U)fov & ~4095u), 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    fov[0] = 0xe3001000 | (b & 0xfff) | ((b >> 12 & 0xf) << 16);
    fov[1] = 0xe3401000 | (b >> 16 & 0xfff) | ((b >> 28) << 16);
    __builtin___clear_cache((char *)fov, (char *)(fov + 2));
    g_ui_zoom = z;
    fprintf(stderr, "qol: larger menus on (zoom %.2f, UI camera fov %.3f)\n", z, f);
}

static void options_install(void) {
    U a_setmenu = sym("_ZN23UIConsoleMenuController7SetMenuEP13UIConsoleMenu");
    U a_opt_event = sym("_ZN24OptionsConsoleController11OnMenuEventEP6UINode14E_UIInputEventRK9GfContext");
    U a_aud_init = sym("_ZN29AudioOptionsConsoleController4InitEv");
    U a_aud_back = sym("_ZN29AudioOptionsConsoleController8MenuBackEv");
    U a_aud_event = sym("_ZN29AudioOptionsConsoleController11OnMenuEventEP6UINode14E_UIInputEventRK9GfContext");
    U vt = sym("_ZTV24OptionsConsoleController");
    f_menu_create = (void *)sym("_ZN25UIConsoleMenuNonScrolling6CreateEP24C_FrontendMenuController");
    f_base_init = (void *)sym("_ZN23UIConsoleMenuController4InitEv");
    f_set_slider = (void *)sym("_ZN8UISlider17SetSliderPositionEf");
    f_get_slider = (void *)sym("_ZNK8UISlider16GetSliderPositonEv");
    f_front_back = (void *)sym("_ZN24C_FrontendMenuController8MenuBackEv");
    f_get_label = (void *)sym("_ZN17UIConsoleMenuItem8GetLabelEv");
    f_set_text = (void *)sym("_ZN7UILabel7SetTextEPKc");
    if (!a_setmenu || !a_opt_event || !a_aud_init || !a_aud_back || !a_aud_event || !vt ||
        !f_menu_create || !f_base_init || !f_set_slider || !f_get_slider || !f_front_back ||
        !f_get_label || !f_set_text) return;
    vt_options = vt + 8;
    snprintf(L_preset[0], sizeof L_preset[0], "PRESET: CUSTOM");
    for (int i = 0; i < NPRESETS; i++) snprintf(L_preset[i + 1], sizeof L_preset[i + 1], "PRESET: %s", kPresets[i].label);
    /* all or nothing: a half-installed page would be worse than none */
    U t[5];
    U addr[5] = {a_setmenu, a_opt_event, a_aud_init, a_aud_back, a_aud_event};
    U fn[5] = {(U)h_setmenu, (U)h_opt_event, (U)h_aud_init, (U)h_aud_back, (U)h_aud_event};
    for (int i = 0; i < 5; i++) if (!make_tramp(addr[i], NULL, 0, "options")) return;
    for (int i = 0; i < 5; i++) t[i] = hook_wrap(addr[i], fn[i], "options");
    o_setmenu = (void *)t[0]; o_opt_event = (void *)t[1]; o_aud_init = (void *)t[2];
    o_aud_back = (void *)t[3]; o_aud_event = (void *)t[4];
    U a;
    if ((a = sym("_ZN16FrontendTitleBar19UpdateMainTitleTextEPKc")))
        o_title = (void *)hook_wrap(a, (U)h_title, "FrontendTitleBar::UpdateMainTitleText");
    if ((a = sym("_ZN17NavigationManager16SetMainTitleTextEPKc20E_TitleBarTransition")))
        o_title2 = (void *)hook_wrap(a, (U)h_title2, "NavigationManager::SetMainTitleText");
    fprintf(stderr, "qol: Options > Graphics installed\n");

    /* achievements list on the Credits screen */
    f_enter = (void *)sym("_ZN14C_MenuSequence9EnterMenuEi");
    g_menuseq = sym("gMenuSequence");
    if ((a = sym("_ZN24CreditsConsoleControllerD2Ev")))
        o_credits_dtor = (void *)hook_wrap(a, (U)h_credits_dtor, "CreditsConsoleController::~");
    g_credits_vec = (U *)so_symbol(&gw3_mod, "creditsVector");
    if ((a = sym("_ZN24CreditsConsoleController4InitEv")) && g_credits_vec)
        o_credits_init = (void *)hook_wrap(a, (U)h_credits_init, "CreditsConsoleController::Init");
    if (!o_credits_dtor || !o_credits_init) f_enter = NULL;   /* couldn't tell when the list closes: keep Credits as is */
    /* The Achievements tile first asks SignedInToAppStore() and otherwise says
     * "You need to be signed in to Google Play".  Answer yes at that call site
     * (1.3.0 build 101; the instruction is checked before patching). */
    U show = so_symbol(&gw3_mod, "_ZN8Services18ShowAchievementsUIEi");
    if (show) {
        uint32_t *at = (uint32_t *)(show - 0x4367a0 + 0x318fdc);
        if (*at == 0xeb0472a3) {
            mprotect((void *)((U)at & ~4095u), 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
            *at = 0xe3a00001;   /* mov r0, #1 */
            __builtin___clear_cache((char *)at, (char *)(at + 1));
            fprintf(stderr, "qol: Achievements tile no longer needs Google Play\n");
        } else fprintf(stderr, "qol: Achievements tile check not found (other game version)\n");
    }
}

/* ── prompts / replacement assets ─────────────────────────────────────────── */

typedef struct { const char *name; unsigned size; const unsigned char *data; } Builtin;
static const Builtin kBuiltins[] = {
#include "prompts.inc"
};

int qol_asset(const char *name, const uint8_t **data, long *len) {
    size_t n = strlen(name);
    if (g_ach_mode && n >= 16 && !strcasecmp(name + n - 16, "gwarscredits.xml")) {
        *len = build_achievements_xml();
        *data = (const uint8_t *)g_xml;
        return 1;
    }
    if (getenv("GW3_STOCK")) return 0;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (size_t i = 0; i < sizeof kBuiltins / sizeof kBuiltins[0]; i++)
        if (!strcasecmp(kBuiltins[i].name, base)) { *data = kBuiltins[i].data; *len = kBuiltins[i].size; return 1; }
    return 0;
}

/* ── install ──────────────────────────────────────────────────────────────── */

static void settings_load(void) {
    char p[600]; ini_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        char k[32]; float v;
        while (fscanf(f, " %31[^=]=%f", k, &v) == 2) {
            if (!strcmp(k, "particles")) g_set.particles = v;
            else if (!strcmp(k, "background")) g_set.background = v;
            else if (!strcmp(k, "fps")) g_set.fps = v != 0;
            else if (!strcmp(k, "nintendo_layout")) g_set.nintendo = v != 0;
            else if (!strcmp(k, "large_menus")) g_set.large_menus = v != 0;
            else if (!strcmp(k, "stage_zoom")) g_set.stage_zoom = v < 0 ? 0 : v > 3 ? 3 : (int)v;
        }
        fclose(f);
    }
    if (getenv("GW3_XBOX_LAYOUT")) g_set.nintendo = 0;
    ach_path(p, sizeof p);
    if ((f = fopen(p, "r"))) {   /* lines: "<index> <date> <name>" */
        int i; char date[16];
        while (fscanf(f, "%d %15s%*[^\n]", &i, date) == 2)
            if (i >= 0 && i < NACH) snprintf(g_ach_date[i], sizeof g_ach_date[i], "%s", date);
        fclose(f);
    }
    fprintf(stderr, "qol: particles %.2f, background %.2f, buttons %s, fps counter %s, %d/%d achievements\n",
            g_set.particles, g_set.background, g_set.nintendo ? "R36S" : "Xbox", g_set.fps ? "on" : "off",
            ach_count(), NACH);
}

/* Audio::C_AudioSystem command methods.  The engine calls them with
 * this == NULL while (or whenever) its audio system doesn't exist; the original
 * port stubbed them all out (no music, no sound effects).  Now they only skip
 * the call when `this` is NULL. */
static const char *kAudioCmds[] = {
    "_ZN5Audio13C_AudioSystem14UpdateListenerENS_16C_ListenerHandleERKN5Maths6MatrixERKNS2_6VectorE",
    "_ZN5Audio13C_AudioSystem14RemoveListenerENS_16C_ListenerHandleE",
    "_ZN5Audio13C_AudioSystem17SetCategoryVolumeEPKcfb",
    "_ZN5Audio13C_AudioSystem15SetCategoryFadeEPKcff",
    "_ZN5Audio13C_AudioSystem21RemovePersistantSoundENS_23C_PersistantSoundHandleE",
    "_ZN5Audio13C_AudioSystem20StartPersistantSoundENS_23C_PersistantSoundHandleEfb",
    "_ZN5Audio13C_AudioSystem19StopPersistantSoundENS_23C_PersistantSoundHandleEfb",
    "_ZN5Audio13C_AudioSystem20PausePersistantSoundENS_23C_PersistantSoundHandleEb",
    "_ZN5Audio13C_AudioSystem27SetPersistantSoundParameterENS_23C_PersistantSoundHandleEPKcf",
    "_ZN5Audio13C_AudioSystem28SetPersistantSoundAttributesENS_23C_PersistantSoundHandleERKN5Maths6VectorES5_",
    "_ZN5Audio13C_AudioSystem28SetPersistantSound3DPanLevelENS_23C_PersistantSoundHandleEf",
    "_ZN5Audio13C_AudioSystem21KeyOffPersistantSoundENS_23C_PersistantSoundHandleEPKc",
    "_ZN5Audio13C_AudioSystem18RemoveOneShotSoundENS_20C_OneShotSoundHandleE",
    "_ZN5Audio13C_AudioSystem19TriggerOneShotSoundENS_20C_OneShotSoundHandleE",
    "_ZN5Audio13C_AudioSystem24SetOneShotSoundParameterENS_20C_OneShotSoundHandleEPKcf",
    "_ZN5Audio13C_AudioSystem25SetOneShotSoundAttributesENS_20C_OneShotSoundHandleERKN5Maths6VectorES5_",
    "_ZN5Audio13C_AudioSystem25SetOneShotSound3DPanLevelENS_20C_OneShotSoundHandleEf",
    "_ZN5Audio13C_AudioSystem27RemoveSimpleSoundCollectionENS_29C_SimpleSoundCollectionHandleE",
    "_ZN5Audio13C_AudioSystem31AddGroupToSimpleSoundCollectionENS_29C_SimpleSoundCollectionHandleEPKc",
    "_ZN5Audio13C_AudioSystem36RemoveGroupFromSimpleSoundCollectionENS_29C_SimpleSoundCollectionHandleEPKc",
    "_ZN5Audio13C_AudioSystem35TriggerEventInSimpleSoundCollectionENS_29C_SimpleSoundCollectionHandleEPKcS3_",
    "_ZN5Audio13C_AudioSystem14RemoveMusicCueENS_16C_MusicCueHandleE",
    "_ZN5Audio13C_AudioSystem13BeginMusicCueENS_16C_MusicCueHandleE",
    "_ZN5Audio13C_AudioSystem11EndMusicCueENS_16C_MusicCueHandleE",
    "_ZN5Audio13C_AudioSystem17SetMusicParameterEPKcf",
};
static int ret0(void) { return 0; }
static void audio_guards(void) {
    int guarded = 0, stubbed = 0, n = (int)(sizeof kAudioCmds / sizeof kAudioCmds[0]);
    for (int i = 0; i < n; i++) {
        const char *name = kAudioCmds[i];
        U a = so_symbol(&gw3_mod, name);
        if (!a) { fprintf(stderr, "qol: audio %s not found\n", name); continue; }
        if (getenv("GW3_NOAUDIO_CMDS")) { hook_addr(a, (U)ret0); stubbed++; }
        else if (hook_null_guard(a, name)) guarded++;
        else { hook_addr(a, (U)ret0); stubbed++; }
    }
    fprintf(stderr, "qol: audio commands: %d NULL-guarded, %d stubbed\n", guarded, stubbed);
}

void qol_install(void) {
    settings_load();
    audio_guards();
    stage_zoom_find();
    stage_zoom_apply();
    U a;
    hook_addr(sym("_ZN8Services24PlayerCanGetAchievementsEi"), (U)ret1);
    hook_addr(sym("_ZN8Services27RegisterAchievementProgressEi13E_Achievementf"), (U)h_ach_progress);
    hook_addr(sym("_ZN8Services18ShowAchievementsUIEi"), (U)h_show_achievements);
    if ((a = sym("_Z17IsJoypadConnectedi"))) hook_addr(a, (U)h_joypad_connected);
    if ((a = sym("_ZN16C_DetailSettings21GetParticleComplexityEv")))
        o_particles = (void *)hook_wrap(a, (U)h_particles, "GetParticleComplexity");
    if ((a = sym("_ZN16C_DetailSettings23GetBackgroundComplexityEv")))
        o_background = (void *)hook_wrap(a, (U)h_background, "GetBackgroundComplexity");
    if (!getenv("GW3_STOCK")) {
        f_set_visible = (void *)sym("_ZN6UINode10SetVisibleEb");
        if (f_set_visible) {
            hook_addr(sym("_ZN21LeaderboardSignInNode6UpdateEf"), (U)h_hide_update);
            hook_addr(sym("_ZN14AppStoreButton6UpdateEf"), (U)h_hide_update);
        }
        if (so_symbol(&gw3_mod, "Java_com_activision_gw3_common_GW3JNILib_setJoystickConnected") &&
            !getenv("GW3_PHONE")) {   /* the console UI is 1.3.0 only */
            ui_zoom_install();
            options_install();
        }
    }
}

/* ── drawing: 8x8 font for the port menu, the game's Futura for the pop-up ── */

static GLuint g_prog, g_tex;
static GLint g_u_screen, g_u_color;
#define KSOLID 128            /* atlas slot that is fully lit (boxes) */
static float g_verts[24 * 4096];
static int g_nv;              /* floats used */

static GLuint shader(GLenum k, const char *src) {
    GLuint s = glCreateShader(k);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}
static void build(void) {
    const char *vs = "attribute vec2 p; attribute vec2 t; uniform vec2 s; varying vec2 v;"
                     "void main(){ v = t; gl_Position = vec4(p.x / s.x * 2.0 - 1.0, 1.0 - p.y / s.y * 2.0, 0.0, 1.0); }";
    const char *fs = "precision mediump float; varying vec2 v; uniform sampler2D f; uniform vec4 c;"
                     "void main(){ gl_FragColor = vec4(c.rgb, c.a * texture2D(f, v).a); }";
    g_prog = glCreateProgram();
    glAttachShader(g_prog, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_prog, 0, "p");
    glBindAttribLocation(g_prog, 1, "t");
    glLinkProgram(g_prog);
    g_u_screen = glGetUniformLocation(g_prog, "s");
    g_u_color = glGetUniformLocation(g_prog, "c");
    static uint8_t px[129 * 8 * 8];   /* 129 glyphs of 8x8 side by side, alpha only */
    for (int g = 0; g < 129; g++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                px[y * 129 * 8 + g * 8 + x] = (g == KSOLID || (font8x8_basic[g < 128 ? g : 0][y] >> x & 1)) ? 255 : 0;
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 129 * 8, 8, 0, GL_ALPHA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void vert6(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    if (g_nv + 24 > (int)(sizeof g_verts / sizeof g_verts[0])) return;
    float v[] = {x0, y0, u0, v0, x1, y0, u1, v0, x1, y1, u1, v1, x0, y0, u0, v0, x1, y1, u1, v1, x0, y1, u0, v1};
    memcpy(g_verts + g_nv, v, sizeof v);
    g_nv += 24;
}
static void quad(float x, float y, float w, float h, int glyph) {
    /* inset u by a hair so nearest sampling never bleeds into the next glyph */
    vert6(x, y, x + w, y + h, (glyph * 8 + 0.01f) / (129 * 8.f), 0, (glyph * 8 + 7.99f) / (129 * 8.f), 1);
}
static void flush(float r, float g, float b, float a) {
    if (!g_nv) return;
    glUniform4f(g_u_color, r, g, b, a);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, g_verts);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, g_verts + 2);
    glDrawArrays(GL_TRIANGLES, 0, g_nv / 4);
    g_nv = 0;
}
static void text(float x, float y, float sz, const char *s) {
    for (; *s; s++, x += sz) if ((unsigned char)*s < 128 && *s != ' ') quad(x, y, sz, sz, *s);
}
static void box(float x0, float y0, float bw, float bh, float sz, float alpha) {
    quad(x0, y0, bw, bh, KSOLID); flush(0.02f, 0.05f, 0.12f, alpha);
    float t = sz / 6;
    quad(x0, y0, bw, t, KSOLID); quad(x0, y0 + bh - t, bw, t, KSOLID);
    quad(x0, y0, t, bh, KSOLID); quad(x0 + bw - t, y0, t, bh, KSOLID);
    flush(0.25f, 0.75f, 1.0f, 1.0f);
}

/* The game's own font: BMFont text descriptor + PNG atlas, from the OBB. */
typedef struct { float x, y, w, h, xo, yo, xa; int ok; } Glyph;
typedef struct { GLuint tex, glow; float sw, sh, line; Glyph g[256]; int tried, ok; } Font;
static Font g_f30, g_f22;

static GLuint upload_png(const uint8_t *png, size_t len) {
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(png, (int)len, &w, &h, &n, 4);
    if (!px) return 0;
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(px);
    return t;
}
static float field(const char *line, const char *key) {
    char k[24]; snprintf(k, sizeof k, " %s=", key);
    const char *p = strstr(line, k);
    return p ? (float)atof(p + strlen(k)) : 0.f;
}
static int load_font(Font *f, const char *name) {
    if (f->tried) return f->ok;
    f->tried = 1;
    char path[128];
    uint8_t *fnt = NULL, *png = NULL; size_t nf = 0, np = 0;
    snprintf(path, sizeof path, "Common/Textures/1080/Fonts/%s.fnt", name);
    int okf = aasset_load(path, &fnt, &nf);
    snprintf(path, sizeof path, "Common/Textures/1080/Fonts/%s.png", name);
    if (!okf || !aasset_load(path, &png, &np)) {
        fprintf(stderr, "qol: font %s not in the OBB\n", name);
        free(fnt); return 0;
    }
    char *txt = malloc(nf + 1);
    memcpy(txt, fnt, nf); txt[nf] = 0;
    for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) {
        char line[512]; snprintf(line, sizeof line, " %s", l);
        if (!strncmp(line, " common ", 8)) {
            f->line = field(line, "lineHeight"); f->sw = field(line, "scaleW"); f->sh = field(line, "scaleH");
        } else if (!strncmp(line, " char ", 6)) {
            int id = (int)field(line, "id");
            if (id > 0 && id < 256)
                f->g[id] = (Glyph){field(line, "x"), field(line, "y"), field(line, "width"), field(line, "height"),
                                   field(line, "xoffset"), field(line, "yoffset"), field(line, "xadvance"), 1};
        }
    }
    free(txt); free(fnt);
    f->tex = upload_png(png, np); free(png);
    snprintf(path, sizeof path, "Common/Textures/1080/Fonts/%s_shadow.png", name);
    if (aasset_load(path, &png, &np)) { f->glow = upload_png(png, np); free(png); }
    f->ok = f->tex && f->sw > 1;
    fprintf(stderr, "qol: game font %s %s\n", name, f->ok ? "loaded" : "failed");
    return f->ok;
}
static float ftext_width(Font *f, const char *s, float k) {
    float w = 0;
    for (; *s; s++) if (f->g[(unsigned char)*s].ok) w += f->g[(unsigned char)*s].xa * k;
    return w;
}
static void ftext(Font *f, float x, float y, float k, const char *s, float r, float g, float b, float a) {
    GLuint layers[2] = {f->glow, f->tex};
    for (int L = 0; L < 2; L++) {
        if (!layers[L]) continue;
        float cx = x;
        for (const char *q = s; *q; q++) {
            const Glyph *gl = &f->g[(unsigned char)*q];
            if (!gl->ok) continue;
            float x0 = cx + gl->xo * k, y0 = y + gl->yo * k;
            vert6(x0, y0, x0 + gl->w * k, y0 + gl->h * k,
                  gl->x / f->sw, gl->y / f->sh, (gl->x + gl->w) / f->sw, (gl->y + gl->h) / f->sh);
            cx += gl->xa * k;
        }
        glBindTexture(GL_TEXTURE_2D, layers[L]);
        if (L == 0) flush(r * 0.5f, g * 0.8f, b, a * 0.55f); else flush(r, g, b, a);
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
}
static void centered(Font *f, int w, float k, const char *s, float y, float r, float g, float b, float a) {
    ftext(f, (w - ftext_width(f, s, k)) / 2, y, k, s, r, g, b, a);
}

/* Achievement pop-up in the style of the game's own pop-ups: dark header
 * strip, translucent blue body, light-cyan frame, Futura with its glow. */
static void draw_popup(int w, int h, uint32_t now) {
    if (!load_font(&g_f30, "futura30base10") || !load_font(&g_f22, "futura22base7")) return;
    float k = fmaxf(h / 1080.f, 0.62f);   /* the R36S panel is small: keep it legible */
    float t = (now - g_pop_start) / 1000.f, left = (g_pop_until - now) / 1000.f;
    float a = fminf(1.0f, fminf(t / 0.25f, left / 0.5f));
    float slide = (1 - fminf(1.0f, t / 0.25f)) * 24 * k;
    float k30 = k * 1.15f, k22 = k * 1.05f;
    const char *head = "ACHIEVEMENT UNLOCKED";
    float tw = fmaxf(ftext_width(&g_f22, head, k22), fmaxf(ftext_width(&g_f30, g_pop_name, k30), ftext_width(&g_f22, g_pop_desc, k22)));
    float pw = fminf(fmaxf(640 * k, tw + 96 * k), w - 8.f), hh = 46 * k, bh = 124 * k;
    float x0 = (w - pw) / 2, y0 = h - 300 * k + slide;
    quad(x0, y0, pw, hh, KSOLID); flush(0.03f, 0.07f, 0.17f, 0.92f * a);
    quad(x0, y0 + hh, pw, bh, KSOLID); flush(0.28f, 0.60f, 0.92f, 0.42f * a);
    float b = fmaxf(2 * k, 1.5f);
    quad(x0, y0, pw, b, KSOLID); quad(x0, y0 + hh + bh - b, pw, b, KSOLID);
    quad(x0, y0, b, hh + bh, KSOLID); quad(x0 + pw - b, y0, b, hh + bh, KSOLID);
    quad(x0, y0 + hh - b / 2, pw, b / 2, KSOLID); flush(0.55f, 0.86f, 1.0f, 0.9f * a);
    centered(&g_f22, w, k22, head, y0 + (hh - g_f22.line * k22) / 2, 0.55f, 0.86f, 1.0f, a);
    centered(&g_f30, w, k30, g_pop_name, y0 + hh + 18 * k, 1, 1, 1, a);
    centered(&g_f22, w, k22, g_pop_desc, y0 + hh + 18 * k + g_f30.line * k30 + 8 * k, 0.88f, 0.93f, 1.0f, a);
}

/* ── port menu (hold SELECT for 1 s) ───────────────────────────────────────── */

enum { B_A = 1 << 0, B_B = 1 << 1, B_UP = 1 << 8, B_DOWN = 1 << 9, B_BACK = 1 << 13 };
enum { ITEM_FPS = NPRESETS, ITEM_BUTTONS, ITEM_SIZE, ITEM_ZOOM, ITEM_ACH, NITEMS };
static int g_open, g_ach_page, g_cursor, g_ach_cursor, g_back_held, g_prev;
static char g_toast[96];
static uint32_t g_toast_until;
static uint32_t g_fps_t0, g_fps_frames; static int g_fps;

static void port_menu_open_achievements(void) {
    pthread_mutex_lock(&g_mx);
    g_open = 1; g_ach_page = 1; g_back_held = -1000;
    pthread_mutex_unlock(&g_mx);
}

int qol_input(int *mask) {
    int m = *mask, pressed = m & ~g_prev;
    static int inputlog = -1;
    if (inputlog < 0) inputlog = getenv("GW3_INPUTLOG") != NULL;
    if (inputlog && m != g_prev) fprintf(stderr, "input: %04x at %u ms\n", m, ticks());
    g_prev = m;
    pthread_mutex_lock(&g_mx);
    if (!g_open) {
        g_back_held = (m & B_BACK) && !(m & (1 << 12)) ? g_back_held + 1 : 0;   /* not START+SELECT (quit) */
        if (g_back_held == 60) {
            g_open = 1; g_ach_page = 0;
            g_cursor = preset_match(g_set.particles, g_set.background);
            if (g_cursor < 0) g_cursor = 0;
            fprintf(stderr, "qol: port menu opened\n");
        }
        pthread_mutex_unlock(&g_mx);
        return 0;
    }
    if (g_ach_page) {
        if (pressed & B_UP) g_ach_cursor = (g_ach_cursor + NACH - 1) % NACH;
        if (pressed & B_DOWN) g_ach_cursor = (g_ach_cursor + 1) % NACH;
        if (pressed & B_B) g_ach_page = 0;
    } else {
        if (pressed & B_UP) g_cursor = (g_cursor + NITEMS - 1) % NITEMS;
        if (pressed & B_DOWN) g_cursor = (g_cursor + 1) % NITEMS;
        if (pressed & B_A) {
            if (g_cursor == ITEM_ACH) g_ach_page = 1;
            else if (g_cursor == ITEM_FPS) { g_set.fps = !g_set.fps; settings_save(); }
            else if (g_cursor == ITEM_BUTTONS) { g_set.nintendo = !g_set.nintendo; settings_save(); }
            else if (g_cursor == ITEM_SIZE) { g_set.large_menus = !g_set.large_menus; settings_save(); }
            else if (g_cursor == ITEM_ZOOM) { g_set.stage_zoom = (g_set.stage_zoom + 1) % 4; stage_zoom_apply(); settings_save(); }
            else {
                g_set.particles = kPresets[g_cursor].particles;
                g_set.background = kPresets[g_cursor].background;
                settings_save();
                g_open = 0;
                snprintf(g_toast, sizeof g_toast, "Effects: %s - from the next level", kPresets[g_cursor].name);
                g_toast_until = ticks() + 3000;
            }
        } else if (pressed & B_B) g_open = 0;
    }
    if (!g_open) g_back_held = -1000;   /* don't reopen until SELECT is released and held again */
    pthread_mutex_unlock(&g_mx);
    *mask = 0;
    return 1;
}

/* Everything the overlay changes, saved before and put back after. */
typedef struct { GLint on, size, type, norm, stride, buf; void *ptr; } Attr;
typedef struct {
    GLint prog, active, tex, abuf, fbo, vp[4], bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
    GLboolean blend, depth, cull, scissor, stencil, cmask[4];
    Attr attr[2];
} GLSave;
static void gl_save(GLSave *s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->prog); glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->abuf); glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->fbo);
    glGetIntegerv(GL_VIEWPORT, s->vp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->bsrc_rgb); glGetIntegerv(GL_BLEND_DST_RGB, &s->bdst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->bsrc_a); glGetIntegerv(GL_BLEND_DST_ALPHA, &s->bdst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->beq_rgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->beq_a);
    s->blend = glIsEnabled(GL_BLEND); s->depth = glIsEnabled(GL_DEPTH_TEST); s->cull = glIsEnabled(GL_CULL_FACE);
    s->scissor = glIsEnabled(GL_SCISSOR_TEST); s->stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->cmask);
    for (int i = 0; i < 2; i++) {
        Attr *a = &s->attr[i];
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->on); glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type); glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->norm);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride); glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buf);
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a->ptr);
    }
}
static void set_cap(GLenum c, GLboolean on) { if (on) glEnable(c); else glDisable(c); }
static void gl_restore(const GLSave *s) {
    for (int i = 0; i < 2; i++) {
        const Attr *a = &s->attr[i];
        glBindBuffer(GL_ARRAY_BUFFER, a->buf);
        glVertexAttribPointer(i, a->size, a->type, (GLboolean)a->norm, a->stride, a->ptr);
        if (a->on) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, s->abuf);
    glUseProgram(s->prog); glBindTexture(GL_TEXTURE_2D, s->tex); glActiveTexture(s->active);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo); glViewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    glBlendFuncSeparate(s->bsrc_rgb, s->bdst_rgb, s->bsrc_a, s->bdst_a);
    glBlendEquationSeparate(s->beq_rgb, s->beq_a);
    set_cap(GL_BLEND, s->blend); set_cap(GL_DEPTH_TEST, s->depth); set_cap(GL_CULL_FACE, s->cull);
    set_cap(GL_SCISSOR_TEST, s->scissor); set_cap(GL_STENCIL_TEST, s->stencil);
    glColorMask(s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3]);
}

static void row(float x0, float y, float bw, float sz, int selected, const char *label) {
    if (selected) { quad(x0 + sz * 0.5f, y - sz * 0.25f, bw - sz, sz * 1.5f, KSOLID); flush(0.2f, 0.5f, 0.9f, 0.45f); text(x0 + sz, y, sz, ">"); }
    text(x0 + sz * 2.5f, y, sz, label);
}

void qol_draw(int w, int h) {
    uint32_t now = ticks();
    static long frame;
    /* debug: GW3_ACHTEST="900:5" unlocks achievement 5 at frame 900 (tests the pop-up and the list) */
    const char *at = getenv("GW3_ACHTEST");
    long f; int idx;
    if (at && sscanf(at, "%ld:%d", &f, &idx) == 2 && f == frame) achievement_unlocked(idx);
    frame++;
    pthread_mutex_lock(&g_mx);
    if (g_pop_pending) { g_pop_pending = 0; g_pop_start = now; g_pop_until = now + 5000; }
    g_fps_frames++;
    if (now - g_fps_t0 >= 1000) { g_fps = (int)(g_fps_frames * 1000 / (now - g_fps_t0 ? now - g_fps_t0 : 1)); g_fps_frames = 0; g_fps_t0 = now; }
    int toast = now < g_toast_until, popup = now < g_pop_until;
    /* the first frame goes through once anyway, to load the game font up front */
    if (!g_open && !toast && !popup && !g_set.fps && g_f22.tried) { pthread_mutex_unlock(&g_mx); return; }
    if (!g_prog) build();
    GLSave s; gl_save(&s);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glUseProgram(g_prog);
    glUniform2f(g_u_screen, (float)w, (float)h);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_tex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glEnable(GL_BLEND); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    float sz = floorf(h / 720.f * 24);   /* 16 px characters on the 480p panel */
    if (sz < 8) sz = 8;
    if (g_open && g_ach_page) {
        const int cols = 38, rows = 16, vis = 10;
        float bw = cols * sz, bh = rows * sz;
        if (bw > w - 8) { sz = floorf((w - 8) / (float)cols); bw = cols * sz; bh = rows * sz; }
        float x0 = (w - bw) / 2, y0 = (h - bh) / 2;
        box(x0, y0, bw, bh, sz, 0.9f);
        char title[64]; snprintf(title, sizeof title, "Achievements  %d/%d", ach_count(), NACH);
        text(x0 + sz, y0 + sz * 0.8f, sz, title); flush(0.4f, 0.85f, 1.0f, 1.0f);
        int first = g_ach_cursor - vis / 2;
        if (first > NACH - vis) first = NACH - vis;
        if (first < 0) first = 0;
        for (int i = first; i < first + vis; i++) {
            float y = y0 + sz * (2.4f + (i - first) * 1.05f);
            int got = g_ach_date[i][0] != 0;
            if (i == g_ach_cursor) { quad(x0 + sz * 0.5f, y - sz * 0.15f, bw - sz, sz * 1.1f, KSOLID); flush(0.2f, 0.5f, 0.9f, 0.45f); }
            text(x0 + sz, y, sz * 0.8f, got ? "*" : "-"); text(x0 + sz * 2.2f, y, sz * 0.8f, kAch[i].name);
            if (got) text(x0 + bw - sz * 9.0f, y, sz * 0.8f, g_ach_date[i]);
            flush(got ? 1.0f : 0.55f, got ? 0.9f : 0.6f, got ? 0.45f : 0.7f, 1);
        }
        text(x0 + sz, y0 + bh - sz * 2.6f, sz * 0.7f, kAch[g_ach_cursor].desc); flush(0.85f, 0.9f, 1.0f, 1);
        text(x0 + sz, y0 + bh - sz * 1.5f, sz * 0.7f, "Up/Down: scroll   B: back"); flush(0.7f, 0.75f, 0.85f, 1);
    } else if (g_open) {
        const int cols = 32, rows = 18;
        float bw = cols * sz, bh = rows * sz;
        if (bw > w - 8) { sz = floorf((w - 8) / (float)cols); bw = cols * sz; bh = rows * sz; }
        float x0 = (w - bw) / 2, y0 = (h - bh) / 2;
        box(x0, y0, bw, bh, sz, 0.88f);
        text(x0 + sz, y0 + sz * 0.8f, sz, "Port menu - Effects detail"); flush(0.4f, 0.85f, 1.0f, 1.0f);
        int cur = preset_match(g_set.particles, g_set.background);
        for (int i = 0; i < NPRESETS; i++) {
            float y = y0 + sz * (2.6f + i * 1.4f);
            row(x0, y, bw, sz, i == g_cursor, kPresets[i].name);
            if (i == cur) text(x0 + bw - sz * 2, y, sz, "*");
            flush(1, 1, 1, 1);
        }
        char t[48];
        float y = y0 + sz * (2.6f + NPRESETS * 1.4f + 0.3f);
        row(x0, y, bw, sz, g_cursor == ITEM_FPS, g_set.fps ? "FPS counter    On" : "FPS counter    Off"); flush(1, 1, 1, 1);
        y += sz * 1.4f;
        row(x0, y, bw, sz, g_cursor == ITEM_BUTTONS, g_set.nintendo ? "Buttons        R36S" : "Buttons        Xbox"); flush(1, 1, 1, 1);
        y += sz * 1.4f;
        row(x0, y, bw, sz, g_cursor == ITEM_SIZE, g_set.large_menus ? "Menu size      Large" : "Menu size      Normal");
        if (g_set.large_menus != (g_ui_zoom > 1.0f)) text(x0 + bw - sz * 3.5f, y, sz * 0.6f, "restart");
        flush(1, 1, 1, 1);
        y += sz * 1.4f;
        snprintf(t, sizeof t, "Stage zoom     %s", g_set.stage_zoom ? kStageZoomName[g_set.stage_zoom] : "Off");
        row(x0, y, bw, sz, g_cursor == ITEM_ZOOM, t); flush(1, 1, 1, 1);
        y += sz * 1.4f;
        snprintf(t, sizeof t, "Achievements %d/%d", ach_count(), NACH);
        row(x0, y, bw, sz, g_cursor == ITEM_ACH, t); flush(1.0f, 0.9f, 0.45f, 1);
        text(x0 + sz, y0 + bh - sz * 2.6f, sz * 0.75f, "Effects apply from the next level"); flush(0.7f, 0.75f, 0.85f, 1);
        text(x0 + sz, y0 + bh - sz * 1.5f, sz * 0.75f, "A: choose   B: close"); flush(0.7f, 0.75f, 0.85f, 1);
    } else if (toast) {
        float tsz = floorf(sz * 0.75f), tw = (strlen(g_toast) + 2) * tsz;
        if (tw > w - 8) { tsz = floorf((w - 8) / (float)(strlen(g_toast) + 2)); tw = (strlen(g_toast) + 2) * tsz; }
        quad((w - tw) / 2, h - tsz * 4, tw, tsz * 2, KSOLID); flush(0.02f, 0.05f, 0.12f, 0.85f);
        text((w - tw) / 2 + tsz, h - tsz * 3.5f, tsz, g_toast); flush(1, 1, 1, 1);
    }
    if (!g_f22.tried) { load_font(&g_f30, "futura30base10"); load_font(&g_f22, "futura22base7"); }
    if (popup) draw_popup(w, h, now);
    if (g_set.fps) {
        char t[16]; snprintf(t, sizeof t, "%d FPS", g_fps);
        float fsz = floorf(sz * 0.6f);
        quad(fsz * 0.5f, fsz * 0.5f, (strlen(t) + 1) * fsz, fsz * 1.5f, KSOLID); flush(0, 0, 0, 0.6f);
        text(fsz, fsz * 0.75f, fsz, t); flush(g_fps >= 55 ? 0.5f : 1.0f, g_fps >= 55 ? 1.0f : 0.7f, 0.5f, 1);
    }
    gl_restore(&s);
    pthread_mutex_unlock(&g_mx);
}
