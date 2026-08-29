FROM docker.io/library/debian:bookworm-slim

# Canonical silvaserver.local validation image. Keep the Clang runtime package
# matched to Debian bookworm's default Clang 14 so ASan/UBSan/TSan link probes
# and instrumented executables use one coherent toolchain. libssl-dev provides
# both OpenSSL headers and pkg-config metadata for HTTPS/WSS builds.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang \
        cmake \
        git \
        libc6-dev \
        libclang-rt-14-dev \
        libssl-dev \
        libstdc++-12-dev \
        lld \
        llvm \
        make \
        ninja-build \
        openssl \
        pkg-config \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["bash"]
