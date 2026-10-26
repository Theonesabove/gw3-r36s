#!/bin/bash
# Geometry Wars 3: Dimensions — PortMaster launcher
# 32-bit (armhf) native port of the Android version, running the original
# libgwnext.so engine through a custom Android-on-Linux loader. You supply the
# game's OBB and the engine/FMOD .so files from the APK; see README.md.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"

# 32-bit (armhf) port: ask PortMaster for its armhf runtime so the game's
# libraries resolve on aarch64-only firmware.
export PORT_32BIT="Y"

[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

ESUDO="${ESUDO:-sudo}"
DEVICE_ARCH="${DEVICE_ARCH:-armhf}"
CFW_NAME="${CFW_NAME:-unknown}"
type get_controls         >/dev/null 2>&1 || get_controls() { :; }
type pm_platform_helper   >/dev/null 2>&1 || pm_platform_helper() { :; }
type pm_finish            >/dev/null 2>&1 || pm_finish() { :; }

get_controls

PORTDIR="$(dirname "$(realpath "$0")")"
GAMEDIR="$PORTDIR/gw3"
CONFDIR="$GAMEDIR/conf"
mkdir -p "$CONFDIR"
cd "$GAMEDIR" || exit 1

CURR_TTY="/dev/tty1"
[ -e "$CURR_TTY" ] || CURR_TTY="/dev/tty0"

> "$GAMEDIR/gw3.log"

$ESUDO chmod 666 "$CURR_TTY"          2>/dev/null
$ESUDO chmod 666 /dev/uinput          2>/dev/null
$ESUDO chmod 666 /dev/dri/card0       2>/dev/null
$ESUDO chmod 666 /dev/dri/renderD128  2>/dev/null

_cleanup() {
    [ -n "$_CLEANED" ] && return
    _CLEANED=1

    if [ -n "$GAME_PID" ] && kill -0 "$GAME_PID" 2>/dev/null; then
        kill -TERM "$GAME_PID" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$GAME_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -0 "$GAME_PID" 2>/dev/null && $ESUDO kill -9 "$GAME_PID" 2>/dev/null
    fi

    $ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null

    echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1
    echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1
    printf "\033c"   > "$CURR_TTY"
    printf "\e[?25h" > "$CURR_TTY"

    $ESUDO systemctl start emulationstation 2>/dev/null || \
      $ESUDO systemctl restart oga_events   2>/dev/null || true

    pm_finish
}
trap _cleanup EXIT
trap '_cleanup; exit 130' INT TERM HUP

sleep 1

printf "\033c"   > "$CURR_TTY"   # clear
printf "\e[?25l" > "$CURR_TTY"   # hide cursor

# Runtime env shared by the game.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

export SDL_VIDEODRIVER=kmsdrm
export SDL_VIDEO_GL_DRIVER=libGLESv2.so
export SDL_VIDEO_EGL_DRIVER=libEGL.so

export LD_LIBRARY_PATH="$GAMEDIR/libs.armhf:/usr/lib/arm-linux-gnueabihf:/usr/lib32:$LD_LIBRARY_PATH"

echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1
echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1
$ESUDO chmod 666 /sys/class/vtconsole/vtcon0/bind 2>/dev/null
$ESUDO chmod 666 /sys/class/vtconsole/vtcon1/bind 2>/dev/null

# ── Auto-extract engine + FMOD libs from an APK/XAPK ──────────────────────
# libgwnext.so / libfmodex.so / libfmodevent.so live only inside the game APK
# (lib/armeabi-v7a/).  Drop the .apk — or an .xapk/.apkm bundle, which also
# carries the OBB — into $GAMEDIR and we unzip them here automatically.
gw3_extract_libs() {   # $1 = apk file
    unzip -o -j "$1" \
        'lib/armeabi-v7a/libgwnext.so' \
        'lib/armeabi-v7a/libfmodex.so' \
        'lib/armeabi-v7a/libfmodevent.so' -d "$GAMEDIR" >/dev/null 2>&1
    chmod 644 "$GAMEDIR"/libgwnext.so "$GAMEDIR"/libfmodex.so \
              "$GAMEDIR"/libfmodevent.so 2>/dev/null
}
if [ ! -f "$GAMEDIR/libgwnext.so" ]; then
    for _apk in "$GAMEDIR"/*.apk; do
        [ -f "$_apk" ] || continue
        echo "gw3: extracting engine libs from $(basename "$_apk")"
        gw3_extract_libs "$_apk"
        [ -f "$GAMEDIR/libgwnext.so" ] && break
    done
fi
if [ ! -f "$GAMEDIR/libgwnext.so" ]; then
    for _bundle in "$GAMEDIR"/*.xapk "$GAMEDIR"/*.apkm; do
        [ -f "$_bundle" ] || continue
        echo "gw3: unpacking bundle $(basename "$_bundle")"
        _tmp="$GAMEDIR/.bundle.$$"
        rm -rf "$_tmp"; mkdir -p "$_tmp" || continue
        unzip -o -q "$_bundle" -d "$_tmp"
        find "$_tmp" -iname '*.obb' -exec cp -n {} "$GAMEDIR/" \;
        find "$_tmp" -iname '*.apk' | while read -r _a; do
            unzip -l "$_a" 2>/dev/null | grep -q 'lib/armeabi-v7a/libgwnext.so' || continue
            gw3_extract_libs "$_a"
            break
        done
        rm -rf "$_tmp"
        [ -f "$GAMEDIR/libgwnext.so" ] && break
    done
fi

# ── First-run data check ───────────────────────────────────────────────────
# The engine reads the OBB directly; we just need the user-supplied files.
OBB_FILE=$(ls "$GAMEDIR"/*.obb 2>/dev/null | head -1)
if [ -z "$OBB_FILE" ] || [ ! -f "$GAMEDIR/libgwnext.so" ] \
   || [ ! -f "$GAMEDIR/libfmodex.so" ] || [ ! -f "$GAMEDIR/libfmodevent.so" ]; then
    exec > >(tee -a "$GAMEDIR/gw3.log" > "$CURR_TTY") 2>&1
    echo "============================================================"
    echo "  Geometry Wars 3 — INSTALLATION INCOMPLETE"
    echo "============================================================"
    echo "Put your own copies of these in:  $GAMEDIR/"
    echo "  - the game APK (Geometry*.apk, .xapk or .apkm) — engine + FMOD"
    echo "    libs are unpacked from it automatically"
    echo "  - the OBB: main.101.com.activision.gw3.dimensions.obb (1.3.0,"
    echo "    recommended) or main.35... (1.0.0); an .xapk/.apkm bundle"
    echo "    already contains it"
    echo "Status:"
    [ -n "$OBB_FILE" ] && echo "  [OK]      OBB found: $(basename "$OBB_FILE")" || echo "  [MISSING] No *.obb file found"
    [ -f "$GAMEDIR/libgwnext.so" ]    && echo "  [OK]      libgwnext.so"    || echo "  [MISSING] libgwnext.so"
    [ -f "$GAMEDIR/libfmodex.so" ]    && echo "  [OK]      libfmodex.so"    || echo "  [MISSING] libfmodex.so"
    [ -f "$GAMEDIR/libfmodevent.so" ] && echo "  [OK]      libfmodevent.so" || echo "  [MISSING] libfmodevent.so"
    echo "Returning to menu in 20s..."
    sleep 20
    exit 1
fi

# ── Game run ───────────────────────────────────────────────────────────────
exec > >(tee -a "$GAMEDIR/gw3.log") 2>&1

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export AUDIODEV="${AUDIODEV:-default}"

# Tell the game binary where its data lives (overrides the compiled-in default).
export GW3_DIR="$GAMEDIR"

GAME_PRELOAD="$GAMEDIR/libclock_fix.so"

USE_GPTOKEYB="${USE_GPTOKEYB:-0}"
if [ "$USE_GPTOKEYB" = "1" ] && [ -n "$GPTOKEYB" ]; then
    $GPTOKEYB "gw3" -c "$GAMEDIR/gw3.gptk" &
fi

pm_platform_helper "$GAMEDIR/gw3"

LD_PRELOAD="$GAME_PRELOAD" ./gw3 &
GAME_PID=$!
wait "$GAME_PID"
