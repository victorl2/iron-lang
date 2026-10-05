/* toolchain.h: the pinned C toolchain ironc compiles generated C with.
 *
 * Iron ships one LLVM release (clang, lld, llvm-ar, llvm-dlltool and the
 * compiler-rt runtimes) as a bundle per host, built by the toolchain
 * workflow and published as release assets. ironc never uses the clang on
 * PATH; it looks for the bundle at
 *
 *   1. $IRON_TOOLCHAIN                      developer override, reported in
 *                                           --version and --verbose output
 *   2. <prefix>/lib/iron/toolchain/         next to the installed binary
 *   3. ~/.iron/toolchain/<version>/         per-user install, downloaded on
 *                                           first use with checksum
 *                                           verification
 *
 * Every bundle carries a toolchain.txt manifest; a bundle whose LLVM or
 * bundle number differs from the one this ironc was built for is refused
 * (the developer override only warns). */
#ifndef IRON_CLI_TOOLCHAIN_H
#define IRON_CLI_TOOLCHAIN_H

#include <stdbool.h>
#include <stdio.h>

#include "cli/toolchain_pins.h"

/* "<llvm>-<bundle>", the directory name under ~/.iron/toolchain/ and the
 * suffix of the release tag and archive names. */
#define IRON_TOOLCHAIN_VERSION IRON_TOOLCHAIN_LLVM "-" IRON_TOOLCHAIN_BUNDLE

typedef enum {
    IRON_TOOLCHAIN_FROM_ENV,      /* $IRON_TOOLCHAIN */
    IRON_TOOLCHAIN_FROM_PREFIX,   /* <prefix>/lib/iron/toolchain */
    IRON_TOOLCHAIN_FROM_HOME      /* ~/.iron/toolchain/<version> */
} IronToolchainSource;

typedef struct {
    char root[4096];              /* bundle directory */
    char llvm[32];                /* from the manifest */
    char bundle[16];
    char host[32];
    IronToolchainSource source;
} IronToolchain;

/* The toolchain this process compiles with. Resolved once; on the first
 * call a missing per-user bundle is downloaded unless `download` is false.
 * Returns NULL after printing the reason (not installed, refused, failed
 * download). */
const IronToolchain *iron_toolchain_get(bool download);

/* Absolute path of a tool in the bundle ("clang", "clang-cl", "llvm-ar",
 * ...), with .exe on Windows. Static storage, valid until the next call. */
const char *iron_toolchain_tool(const IronToolchain *tc, const char *name);

/* macOS only: the platform SDK the runtime sources are compiled against
 * until the runtime ships precompiled ($SDKROOT, else `xcrun
 * --show-sdk-path`). NULL elsewhere or when no SDK is found. */
const char *iron_toolchain_sysroot(void);

/* The host this binary runs on, as the bundle archives name it:
 * macos-arm64, macos-x86_64, linux-x86_64, linux-arm64, windows-x86_64. */
const char *iron_toolchain_host(void);

/* One line for --version: the pinned version, and where the bundle was
 * found or that it is not installed. Never downloads. */
void iron_toolchain_print_version(FILE *out);

/* `ironc toolchain <path|install|info|check>`. argv[0] is the subcommand name. */
int iron_toolchain_cmd(int argc, char **argv);

/* The install prefix this binary runs from (<prefix>/bin/ironc -> <prefix>),
 * and the user's home directory (HOME, or USERPROFILE on Windows). */
int iron_toolchain_prefix_dir(char *out, size_t cap);
const char *iron_toolchain_home_dir(void);

/* Run a program found on PATH and return its exit status, or -1 when it
 * could not be started. */
int iron_toolchain_run(char *const argv[]);

/* Download `url` as `archive_name`, verify it against `pinned_sha` (hex) or,
 * when that is NULL, against the hash in the .sha256 sidecar at
 * `sidecar_url`, and unpack it into `dest` (a directory, replaced). `label`
 * names what is being fetched in the progress line. Shared by the
 * toolchain and the per-target runtime bundles (rtbundle.c). */
int iron_toolchain_fetch_archive(const char *url, const char *archive_name,
                                 const char *pinned_sha, const char *sidecar_url,
                                 const char *dest, const char *label);

#endif /* IRON_CLI_TOOLCHAIN_H */
