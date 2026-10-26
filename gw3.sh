#!/bin/bash
# Geometry Wars 3: Dimensions launcher for R36S / ArkOS / NextOS (PortMaster-compatible)

CURR_TTY="/dev/tty1"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

# PortMaster integration
for pmdir in "/opt/system/Tools/PortMaster" "/opt/tools/PortMaster" \
             "$XDG_DATA_HOME/PortMaster" "/roms/ports/PortMaster"; do
  if [ -f "$pmdir/control.txt" ]; then
    source "$pmdir/control.txt"
    source "$pmdir/device_info.txt" 2>/dev/null || true
    get_controls 2>/dev/null || true
    break
  fi
done

# Fallbacks when running outside PortMaster (direct terminal / SSH)
ESUDO="${ESUDO:-}"
sdl_controllerconfig="${sdl_controllerconfig:-}"
pm_finish() { :; }

GAMEDIR="/roms/ports/gw3"
cd "$GAMEDIR" || exit 1

# Truncate the log (we'll append to it later, via tee).
> "$GAMEDIR/gw3.log"

# ── TTY ownership & EmulationStation shutdown ──────────────────────────────
$ESUDO chmod 666 $CURR_TTY            2>/dev/null
$ESUDO chmod 666 /dev/uinput          2>/dev/null
$ESUDO chmod 666 /dev/dri/card0       2>/dev/null
$ESUDO chmod 666 /dev/dri/renderD128  2>/dev/null

$ESUDO pkill -TERM emulationstation 2>/dev/null || true
sleep 1
$ESUDO pkill -9    emulationstation 2>/dev/null || true

printf "\033c"   > $CURR_TTY    # clear
printf "\e[?25l" > $CURR_TTY    # hide cursor

# ── Auto-extract engine + FMOD libs from an APK/XAPK ──────────────────────
# libgwnext.so / libfmodex.so / libfmodevent.so live only inside the game APK
# (lib/armeabi-v7a/).  Drop the .apk — or an .xapk/.apkm bundle, which also
# carries the OBB — into $GAMEDIR and we unzip them here so the user never has
# to do it by hand.
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

# ── First-run check ───────────────────────────────────────────────────────
# Geometry Wars 3 needs: the OBB (game data), the engine .so, and the FMOD libs.
OBB_FILE=$(ls "$GAMEDIR"/*.obb 2>/dev/null | head -1)
if [ -z "$OBB_FILE" ] || [ ! -f "$GAMEDIR/libgwnext.so" ] \
   || [ ! -f "$GAMEDIR/libfmodex.so" ] || [ ! -f "$GAMEDIR/libfmodevent.so" ]; then
    exec > >(tee -a "$GAMEDIR/gw3.log" > $CURR_TTY) 2>&1
    echo ""
    echo "============================================================"
    echo "  Geometry Wars 3 — INSTALLATION INCOMPLETE"
    echo "============================================================"
    echo ""
    echo "Put your own copies of these in:  $GAMEDIR/"
    echo ""
    echo "  - the game APK  (Geometry*.apk, .xapk or .apkm) — the engine"
    echo "    and FMOD libraries are unpacked from it automatically"
    echo "  - main.101.com.activision.gw3.dimensions.obb (1.3.0) or main.35... (1.0.0)  (game data;"
    echo "    an .xapk / .apkm bundle already contains this)"
    echo ""
    echo "Status:"
    [ -n "$OBB_FILE" ] && echo "  [OK]      OBB found: $(basename "$OBB_FILE")" \
                       || echo "  [MISSING] No *.obb file found"
    [ -f "$GAMEDIR/libgwnext.so" ]    && echo "  [OK]      libgwnext.so"    || echo "  [MISSING] libgwnext.so"
    [ -f "$GAMEDIR/libfmodex.so" ]    && echo "  [OK]      libfmodex.so"    || echo "  [MISSING] libfmodex.so"
    [ -f "$GAMEDIR/libfmodevent.so" ] && echo "  [OK]      libfmodevent.so" || echo "  [MISSING] libfmodevent.so"
    echo ""
    echo "Returning to menu in 20 seconds..."
    sleep 20
    $ESUDO systemctl start emulationstation 2>/dev/null || true
    pm_finish
    exit 1
fi

# From here on (game run): log only, no more TTY output (game owns the FB).
exec > >(tee -a "$GAMEDIR/gw3.log") 2>&1

# Release VT console framebuffer so KMS/DRM is free for the game
echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1 || true
echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1 || true

# Video
export SDL_VIDEODRIVER=kmsdrm
export SDL_VIDEO_GL_DRIVER=libGLESv2.so
export SDL_VIDEO_EGL_DRIVER=libEGL.so

# Audio — ALSA only on ArkOS (FMOD Android build will try OpenSL and fall back)
export SDL_AUDIODRIVER=alsa
export AUDIODEV=default

# Controller
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Libraries
export LD_LIBRARY_PATH="/usr/lib/arm-linux-gnueabihf:$LD_LIBRARY_PATH"

# Preload our __clock_gettime64 override (bionic/old-glibc time64 mismatch)
export LD_PRELOAD="$GAMEDIR/libclock_fix.so"

./gw3

# Restore VT console and restart frontend
echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1 || true
echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1 || true
printf "\033c" > $CURR_TTY
printf "\e[?25h" > $CURR_TTY

$ESUDO systemctl start emulationstation 2>/dev/null || \
  $ESUDO systemctl restart oga_events 2>/dev/null || true

pm_finish
