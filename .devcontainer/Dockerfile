FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Taipei

# System build deps
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    libssl-dev \
    uuid-dev \
    libbsd-dev \
    capnproto \
    libcapnp-dev \
    cmake-format \
    clang-format \
    curl \
    git \
    wget \
    && rm -rf /var/lib/apt/lists/*

# CMake
RUN wget -qO- https://github.com/Kitware/CMake/releases/download/v4.3.1/cmake-4.3.1-linux-x86_64.tar.gz \
    | tar xzf - -C /usr/local --strip-components=1

# Aeron C client + media driver
RUN git clone --depth 1 https://github.com/real-logic/aeron.git /tmp/aeron && \
    cd /tmp/aeron && \
    mkdir build && cd build && \
    cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_AERON_DRIVER=ON \
      -DAERON_TESTS=OFF \
      -DAERON_SYSTEM_TESTS=OFF \
      -DAERON_BUILD_SAMPLES=OFF \
      -DAERON_BUILD_DOCUMENTATION=OFF \
      -DC_CLIENT_TIMEOUT=OFF \
      -DBUILD_AERON_ARCHIVE_API=OFF \
      .. && \
    make -j$(nproc) aeron aeron_driver aeronmd && \
    make install && \
    ldconfig && \
    rm -rf /tmp/aeron
