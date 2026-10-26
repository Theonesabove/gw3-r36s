#ifndef CONFIG_H
#define CONFIG_H

/* Geometry Wars 3: Dimensions — native ARMv7 loader (NextOS-style shim) */

/* Fallback render resolution ONLY. At runtime main() overwrites g_screen_w/h
 * with the SDL window's real size (FULLSCREEN_DESKTOP = native panel mode) and
 * jni_patch.c feeds that to the engine, so it renders at native res/aspect on
 * whatever panel it lands on. These values are used only if the SDL query
 * fails. GW3's internal design space is 1280x720. */
#define SCREEN_W    1024
#define SCREEN_H    768

/* Path to extracted game data on the device (APK lib + OBB). */
#define DATA_PATH   "/roms/ports/gw3"
#define SO_PATH     DATA_PATH "/libgwnext.so"

/* The OBB is exposed to the engine at this path. It is a custom packed
 * container (WAD/FSB/BIG magic), not a zip — the engine reads it directly. */
#define OBB_RELPATH "/main.35.com.activision.gw3.dimensions.obb"

/* Device memory in MB (used for the JNI GetDeviceType/GetTotalMemory calls). */
#define DEVICE_MEMORY_MB 512

/* Deadzone for analog sticks (0.0–1.0) */
#define STICK_DEADZONE 0.25f

/* #define DEBUG */

#endif /* CONFIG_H */
