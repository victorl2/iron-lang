/* target.h: the targets `ironc build --target=<name>` can produce code for.
 *
 * The native target compiles the runtime from source with the host SDK as
 * before. A cross target compiles the (freestanding) generated C with the
 * pinned clang for the target's triple and links it with lld against the
 * target's runtime bundle (rtbundle.h): the Iron runtime precompiled for
 * the target, a static libc and the compiler builtins. No SDK is involved,
 * so any host builds for any target that has a bundle. */
#ifndef IRON_CLI_TARGET_H
#define IRON_CLI_TARGET_H

#include <stdbool.h>

typedef enum {
    IRON_OS_LINUX,
    IRON_OS_MACOS,
    IRON_OS_WINDOWS
} IronTargetOs;

typedef struct IronCrossTarget {
    const char *name;     /* --target value and bundle name: linux-x86_64 */
    const char *triple;   /* clang --target */
    IronTargetOs os;
    const char *arch;     /* x86_64, aarch64 */
    bool        available; /* a runtime bundle is produced for it */
} IronCrossTarget;

/* The table of cross targets; NULL for an unknown name. */
const IronCrossTarget *iron_target_lookup(const char *name);

/* The comma-separated names, for help and error text. */
const char *iron_target_names(void);

/* True when `name` is this host (as the toolchain bundles name it). */
bool iron_target_is_host(const char *name);

#endif /* IRON_CLI_TARGET_H */
