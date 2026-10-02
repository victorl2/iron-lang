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
    { "macos-arm64",    "" },
    { "macos-x86_64",   "" },
    { "linux-x86_64",   "" },
    { "linux-arm64",    "" },
    { "windows-x86_64", "" },
};

#endif /* IRON_CLI_TOOLCHAIN_PINS_H */
