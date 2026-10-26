#!/usr/bin/env bash
# cross-build.sh -- build gw3 (+ libclock_fix.so) for armhf without Docker or
# armhf dev packages: glibc 2.31 from Ubuntu 20.04's cross packages
# (libc6-dev-armhf-cross, archive.ubuntu.com), the host's arch-independent
# SDL2/GLES/EGL/zlib headers, and link-only stub libraries generated from the
# symbols the objects import (the device supplies the real ones).
#   usage: scripts/cross-build.sh [sysroot-dir]      (default: ./focal-sysroot)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SR="${1:-$HERE/focal-sysroot}"
CC=arm-linux-gnueabihf-gcc
LIBC="$SR/x/usr/arm-linux-gnueabihf"

if [ ! -f "$LIBC/lib/libc.so.6" ]; then
    mkdir -p "$SR/debs" "$SR/x"
    B=http://archive.ubuntu.com/ubuntu/pool/main/c/cross-toolchain-base
    for p in libc6-armhf-cross_2.31-0ubuntu9.9cross1_all.deb libc6-dev-armhf-cross_2.31-0ubuntu9.9cross1_all.deb \
             linux-libc-dev-armhf-cross_5.4.0-110.124cross1_all.deb; do
        [ -f "$SR/debs/$p" ] || curl -fsSL -o "$SR/debs/$p" "$B/$p"
        dpkg-deb -x "$SR/debs/$p" "$SR/x"
    done
fi
INC="$SR/devinc"
if [ ! -d "$INC/SDL2" ]; then
    mkdir -p "$INC"
    cp -r /usr/include/SDL2 /usr/include/GLES2 /usr/include/EGL /usr/include/KHR "$INC/"
    cp /usr/include/*-linux-gnu/SDL2/_real_SDL_config.h "$INC/SDL2/"
    cp /usr/include/zlib.h /usr/include/zconf.h "$INC/"
    sed -i "s/^#define HAVE_IMMINTRIN_H 1/\/* x86 only *\//" "$INC/SDL2/_real_SDL_config.h"
fi

GCC_INC=$($CC -print-file-name=include)
GCC_FIXINC=$($CC -print-file-name=include-fixed)
CFLAGS=(-march=armv7-a -mfpu=neon -mfloat-abi=hard -O1 -g -fno-omit-frame-pointer -DDEBUG
        -Wall -Wno-unused-function -nostdinc -I "$HERE/src"
        -isystem "$GCC_INC" -isystem "$GCC_FIXINC" -isystem "$LIBC/include" -idirafter "$INC")
OUT="$HERE/build-armhf"; mkdir -p "$OUT/stubs"
OBJS=()
for c in main clock_fix so_util jni_patch opengl_patch aasset_patch fmod_patch fmod_opensl qol; do
    $CC "${CFLAGS[@]}" -c -o "$OUT/$c.o" "$HERE/src/$c.c"
    OBJS+=("$OUT/$c.o")
done

# link-only stubs: every symbol the objects import that glibc doesn't define
nm -D --defined-only "$LIBC"/lib/{libc.so.6,libm.so.6,libdl.so.2,libpthread.so.0,librt.so.1} 2>/dev/null \
    | awk '{print $NF}' | sed 's/@.*//' | sort -u > "$OUT/stubs/libc.syms"
arm-linux-gnueabihf-nm --defined-only "${OBJS[@]}" | awk 'NF==3{print $3}' >> "$OUT/stubs/libc.syms"
sort -u -o "$OUT/stubs/libc.syms" "$OUT/stubs/libc.syms"
arm-linux-gnueabihf-nm -u "${OBJS[@]}" | awk 'NF==2{print $2}' | sort -u | comm -23 - "$OUT/stubs/libc.syms" > "$OUT/stubs/need.syms"
mk() {   # mk <soname> <link name> <regex>
    grep -E "$3" "$OUT/stubs/need.syms" | sed 's/.*/void &(void){}/' > "$OUT/stubs/$2.c" || true
    $CC -shared -nostdlib -o "$OUT/stubs/lib$2.so" -Wl,-soname,$1 "$OUT/stubs/$2.c"
}
mk libSDL2-2.0.so.0 SDL2 '^SDL_'
mk libEGL.so.1 EGL '^egl'
mk libGLESv2.so.2 GLESv2 '^gl[A-Z]'
mk libstdc++.so.6 stdc++ '^_Z'   # nothing is imported; kept loaded for the game's C++ runtime imports
mk libz.so.1 z '^(inflate|deflate|crc32|adler32|compress|uncompress|zlib|gz)'
grep -vE '^(SDL_|egl|gl[A-Z]|inflate|deflate|crc32|adler32|compress|uncompress|zlib|gz|_GLOBAL_OFFSET_TABLE_|__aeabi|__gnu|__cxa_begin_cleanup|__cxa_type_match)' \
    "$OUT/stubs/need.syms" | sed 's/^/  unresolved: /' || true

$CC -march=armv7-a -mfpu=neon -mfloat-abi=hard -o "$HERE/gw3.pm" "${OBJS[@]}" \
    --sysroot="$SR/x" -B"$LIBC/lib/" -L"$LIBC/lib" -L"$OUT/stubs" \
    -Wl,--no-as-needed -lSDL2 -lEGL -lGLESv2 -lstdc++ -lz -ldl -lpthread -lm
$CC -O1 -fPIC -shared -nostdlib -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -Wl,-soname,libclock_fix.so -Wl,--version-script="$HERE/src/clock_preload.map" \
    -o "$HERE/libclock_fix.so" "$HERE/src/clock_preload.c"
echo "=== gw3.pm needs: $(arm-linux-gnueabihf-objdump -T "$HERE/gw3.pm" | grep -oE '(GLIBC|GCC)_[0-9.]+' | sort -uV | tr '\n' ' ')"
arm-linux-gnueabihf-readelf -d "$HERE/gw3.pm" | grep NEEDED
