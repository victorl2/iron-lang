#include "cli/target.h"
#include "cli/toolchain.h"
#include <string.h>

static const IronCrossTarget k_targets[] = {
    { "linux-x86_64",   "x86_64-linux-musl",       IRON_OS_LINUX,   "x86_64",  true  },
    { "linux-arm64",    "aarch64-linux-musl",      IRON_OS_LINUX,   "aarch64", true  },
    { "macos-arm64",    "arm64-apple-macosx11.0",  IRON_OS_MACOS,   "aarch64", false },
    { "macos-x86_64",   "x86_64-apple-macosx11.0", IRON_OS_MACOS,   "x86_64",  false },
    { "windows-x86_64", "x86_64-pc-windows-msvc",  IRON_OS_WINDOWS, "x86_64",  false },
};

const IronCrossTarget *iron_target_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(k_targets) / sizeof(k_targets[0]); i++)
        if (strcmp(k_targets[i].name, name) == 0) return &k_targets[i];
    return NULL;
}

const char *iron_target_names(void) {
    return "linux-x86_64, linux-arm64 (macos-arm64, macos-x86_64 and windows-x86_64 "
           "are not available yet)";
}

bool iron_target_is_host(const char *name) {
    const char *host = iron_toolchain_host();
    return name && host && strcmp(name, host) == 0;
}
