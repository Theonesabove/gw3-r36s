# Dockerfile -- armhf (ARMv7) cross-build environment for the GW3 port
#
# The Makefile compiles with arm-linux-gnueabihf-gcc and expects dev headers/libs
# under an "armhf-sysroot/root" prefix. In this container we install the armhf
# packages directly into the system (multiarch) and override SYSROOT=/ when
# invoking make, so /usr/include and /usr/lib/arm-linux-gnueabihf resolve.

# NOTE: target device (ArkOS) ships glibc 2.30, so we must
# build against an OLDER glibc (buster = 2.28). Building against bookworm (2.36)
# produces a binary that requires GLIBC_2.33/2.34 and fails to load on device.
FROM debian:buster

ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture armhf && \
    echo 'deb [trusted=yes] http://archive.debian.org/debian buster main' > /etc/apt/sources.list && \
    echo 'deb [trusted=yes] http://archive.debian.org/debian-security buster/updates main' >> /etc/apt/sources.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        make \
        curl \
        gcc-arm-linux-gnueabihf \
        g++-arm-linux-gnueabihf \
        binutils-arm-linux-gnueabihf \
        libc6-dev:armhf \
        libstdc++-8-dev:armhf \
        libsdl2-dev:armhf \
        libgles2-mesa-dev:armhf \
        libegl1-mesa-dev:armhf \
        libz-dev:armhf \
        unzip && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build
