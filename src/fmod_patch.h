#ifndef FMOD_PATCH_H
#define FMOD_PATCH_H

#include "so_util.h"

/* Loads the game's bundled libfmodex.so + libfmodevent.so (FMOD Ex 4.44.50,
 * Android armeabi-v7a) with the custom ELF loader so libgwnext.so's FMOD
 * imports bind to real code. Returns 0 on success, -1 if the libraries are
 * missing (caller should then keep the game running silently by stubbing
 * Audio::Init / Audio::Update).
 *
 * FMOD's OpenSL ES output is redirected to SDL2 audio via the fake
 * "libOpenSLES.so" in fmod_opensl.c (see main.c's fmod_dlopen). */
int fmod_preload(const char *data_path, so_module *fm, so_module *fme,
                 so_default_dynlib *dynlib, int dynlib_size);

/* fmod_opensl.c -- fake libOpenSLES.so on SDL2. */
void  fmod_opensl_init(void);              /* build vtables (idempotent)     */
void *fmod_opensl_sym(const char *name);   /* dlsym() for the OpenSL shim    */

#endif /* FMOD_PATCH_H */
