# Build image for the host-native SPDK fork (vfio-user target + initiator).
# Ubuntu 22.04 = same glibc and OpenSSL 3 as the host, so binaries run natively.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc g++ make pkg-config nasm yasm autoconf automake libtool \
      python3 python3-pip python3-pyelftools meson ninja-build cmake git ca-certificates rsync \
      libnuma-dev libaio-dev uuid-dev libssl-dev libjson-c-dev libcmocka-dev \
      libfuse3-dev libkeyutils-dev libncurses-dev libunwind-dev \
    && rm -rf /var/lib/apt/lists/*
