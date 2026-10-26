#!/bin/bash
# docker-make.sh -- build the GW3 armhf binary inside a Docker container.
#
# Usage:
#   ./docker-make.sh            # build gw3 + libclock_fix.so
#   ./docker-make.sh clean      # make clean
#   ./docker-make.sh portmaster # build the old-glibc (bullseye) binary too
#   ./docker-make.sh shell      # drop into the build container for debugging
#
# The cross toolchain lives in the image; the source is mounted read-write so
# the produced binaries land in this directory. SYSROOT=/ points the Makefile
# at the container's native armhf multiarch paths.

set -euo pipefail

IMAGE=geowars3-build
DIR="$(cd "$(dirname "$0")" && pwd)"

build_image() {
    docker build -t "$IMAGE" -f "$DIR/Dockerfile" "$DIR"
}

if [ "${1:-}" = "shell" ]; then
    build_image
    exec docker run --rm -it -v "$DIR":/build -w /build "$IMAGE" bash
fi

build_image
# SYSROOT=/ so the Makefile's -I/-L point at /usr/include and
# /usr/lib/arm-linux-gnueabihf (where the armhf dev packages install).
docker run --rm -v "$DIR":/build -w /build "$IMAGE" \
    make SYSROOT=/ "$@"
