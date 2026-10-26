#ifndef QOL_H
#define QOL_H
/* qol.c -- quality-of-life layer on top of the engine (ported from gw3nx):
 *   Options > Graphics page, offline achievements (native list + pop-up),
 *   R36S button layout, port menu (hold SELECT), FPS counter, own prompt
 *   images, sign-in prompts hidden, NULL-safe audio commands. */
#include <stdint.h>
#include <stddef.h>

void qol_install(void);            /* from patch_gw3(), before the engine starts */
int  qol_nintendo_layout(void);    /* 1: A = right button (R36S labels), 0: Xbox positions */
int  qol_input(int *engine_mask);  /* once per frame; 1 = port menu is up, mask cleared */
void qol_draw(int w, int h);       /* after the game's frame, before the swap */
/* Replacement asset (our prompts, the achievements list): 1 = data and len set */
int  qol_asset(const char *norm_name, const uint8_t **data, long *len);

/* hook_wrap: redirect the ARM function at addr to fn and return a callable
 * copy of the original (0 if its first two instructions can't be moved).
 * hook_null_guard: let the function run only when its `this` (r0) is set. */
uintptr_t hook_wrap(uintptr_t addr, uintptr_t fn, const char *name);
int       hook_null_guard(uintptr_t addr, const char *name);

/* aasset_patch.c: a whole file from the OBB (malloc'd), 0 if missing */
int aasset_load(const char *path, uint8_t **out, size_t *len);

#endif
