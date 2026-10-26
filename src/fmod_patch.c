/* fmod_patch.c -- load the game's bundled FMOD Ex / FMOD Event libraries.
 *
 * Geometry Wars 3 links libfmodevent.so (which NEEDs libfmodex.so), both
 * armeabi-v7a Android builds of FMOD Ex 4.44.50. We map them with the same
 * custom ELF loader used for libgwnext.so; libgwnext's 35 FMOD imports then
 * bind to the real code through so_resolve_link (DT_NEEDED soname match).
 *
 * FMOD Ex 4.44 on Android has only these output backends compiled in:
 * OpenSL ES, Java AudioTrack, NoSound, WavWriter. AudioTrack needs a JVM;
 * ALSA/PulseAudio aren't present. So FMOD auto-selects the OpenSL output,
 * which dlopen()s "libOpenSLES.so" and dlsym()s slCreateEngine + five
 * SL_IID_* data symbols. main.c's fmod_dlopen() intercepts that and returns
 * fmod_opensl.c -- a fake libOpenSLES backed by an SDL2 audio device.
 *
 * Reverse-engineered call path (libgwnext.so, 2026-08-30):
 *   Audio::Init @0x22cb58 -> job -> C_AudioSystem::Initialise @0x2245dc:
 *     FMOD_Memory_Initialize(10.5 MB pool)
 *     FMOD_EventSystem_Create ; EventSystem::getSystemObject
 *     System::getNumDrivers  (== 0 -> setOutput(NOSOUND) fallback)
 *     EventSystem::getMusicSystem ; System::setSpeakerMode(STEREO)
 *     EventSystem::init(128, 0x80, NULL, 0)
 *     System::setFileSystem(C_AudioSystem::File{Open,Close,Read,Seek,...})
 *       -> LogicalFS_OpenBundleFile -> the game's WAD mount (NOT libc fopen)
 *     EventSystem::setMediaPath("audio/")
 *   C_AudioSystem::LoadProject("neon.fev")  (project + neon_music_bank.fsb +
 *     neon_sfx_bank.fsb live in the OBB under android/audio/)
 *   Per frame: C_AudioSystem::Update -> EventSystem::update().
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "so_util.h"
#include "fmod_patch.h"

extern so_module gw3_mod;   /* for so_flush_caches on the FMOD modules */

int fmod_preload(const char *data_path, so_module *fm, so_module *fme,
                 so_default_dynlib *dynlib, int dynlib_size)
{
    char path[600];

    fmod_opensl_init();   /* build the SDL2-backed OpenSL shim tables */

    snprintf(path, sizeof(path), "%s/libfmodex.so", data_path);
    if (so_load(fm, path) < 0) {
        fprintf(stderr, "fmod_preload: %s not found -- audio disabled (silent)\n", path);
        return -1;
    }
    so_relocate(fm);
    so_resolve(fm, dynlib, dynlib_size, 0);
    so_flush_caches(fm);
    so_initialize(fm);
    fprintf(stderr, "fmod_preload: libfmodex.so loaded\n");

    snprintf(path, sizeof(path), "%s/libfmodevent.so", data_path);
    if (so_load(fme, path) < 0) {
        fprintf(stderr, "fmod_preload: %s not found -- audio disabled (silent)\n", path);
        return -1;
    }
    so_relocate(fme);
    so_resolve(fme, dynlib, dynlib_size, 0);
    so_flush_caches(fme);
    so_initialize(fme);
    fprintf(stderr, "fmod_preload: libfmodevent.so loaded -- FMOD audio armed\n");

    return 0;
}
