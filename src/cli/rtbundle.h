/* rtbundle.h: the per-target runtime bundle a cross build links against.
 *
 * A bundle (scripts/rt/build_*_bundle.sh, published with each release as
 * iron-rt-<version>-<target>.tar.gz plus a .sha256 sidecar) holds, under
 * lib/, the Iron runtime and stdlib compiled for the target
 * (libiron_rt.a), the target's static libc with its start files and the
 * compiler builtins, and an rt.txt manifest:
 *
 *   iron-rt 1
 *   version <iron version>
 *   target <name>
 *   triple <clang triple>
 *   libc <name> <version>
 *   llvm <version>
 *
 * ironc looks for the bundle of a target at
 *
 *   1. $IRON_RT_DIR/<target>/              developer override (a directory
 *                                          holding one directory per target)
 *   2. <prefix>/lib/iron/rt/<target>/      next to the installed binary
 *   3. ~/.iron/rt/<version>/<target>/      per-user, downloaded on first use
 *
 * A bundle built for another Iron version is refused: the runtime and the
 * generated C must agree on every type and symbol. */
#ifndef IRON_CLI_RTBUNDLE_H
#define IRON_CLI_RTBUNDLE_H

#include <stdbool.h>
#include "cli/target.h"

typedef struct {
    char        root[4096];     /* bundle directory */
    char        lib[4096];      /* <root>/lib */
    char        version[64];    /* from the manifest */
    char        libc[64];       /* "musl 1.2.5" */
    const IronCrossTarget *target;
} IronRtBundle;

/* The bundle for `target`. On the first call a missing per-user bundle is
 * downloaded unless `download` is false. NULL after printing the reason. */
const IronRtBundle *iron_rt_bundle_get(const IronCrossTarget *target, bool download);

/* The bundle for `target` if one is installed (IRON_RT_DIR, next to the
 * compiler or per user), without downloading and without a message when
 * there is none. A bundle that is present but refused is still reported. */
const IronRtBundle *iron_rt_bundle_find(const IronCrossTarget *target);

#endif /* IRON_CLI_RTBUNDLE_H */
