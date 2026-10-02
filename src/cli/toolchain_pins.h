/* toolchain_pins.h: the one toolchain this compiler accepts.
 *
 * Bumping the toolchain is a change to this file: the LLVM release, the
 * bundle number (incremented when the bundle contents change for the same
 * LLVM) and the SHA-256 of each host archive, copied from the
 * SHA256SUMS the toolchain workflow publishes with the release
 * `toolchain-<llvm>-<bundle>`. An empty checksum means that host has no
 * bundle yet; ironc then refuses to download for it. */
#ifndef IRON_CLI_TOOLCHAIN_PINS_H
#define IRON_CLI_TOOLCHAIN_PINS_H

#define IRON_TOOLCHAIN_LLVM   "23.1.2"
#define IRON_TOOLCHAIN_BUNDLE "1"

/* Base URL of the release assets; the archive name is appended. */
#define IRON_TOOLCHAIN_URL \
    "https://github.com/victorl2/iron-lang/releases/download/toolchain-" \
    IRON_TOOLCHAIN_LLVM "-" IRON_TOOLCHAIN_BUNDLE "/"

typedef struct {
    const char *host;
    const char *sha256;   /* 64 lowercase hex digits */
} IronToolchainPin;

static const IronToolchainPin IRON_TOOLCHAIN_PINS[] = {
    { "macos-arm64",    "3fafc1b354efe46e7b2d0186d0ae36d6050916f7ec813765dfd8785ce4aa9ed4" },
    { "macos-x86_64",   "622a576e29e35ca8e5293d980ba7788247e92a6d690ba0af0dd9d4a6d862d1bc" },
    { "linux-x86_64",   "c2d046a7d5ce6a1d57aa6b54bba355fc1f073c3bc22b0544bc312e3534832acd" },
    { "linux-arm64",    "5243292407b6acd16ceac64aede8ae3f75a71950612a0e0933a78acef8fb32bc" },
    { "windows-x86_64", "6e1e8a4466cd962437ebbe047433c6a52404f6a8d4ddd37da8c8d8f43d66acce" },
};

#endif /* IRON_CLI_TOOLCHAIN_PINS_H */
