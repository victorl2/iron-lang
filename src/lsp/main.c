/* iron-lsp entry point — Phase 2 Plan 01 skeleton.
 * Extended by Plan 02 (transport init), Plan 03 (dispatch), Plan 06
 * (sigaction / log / XDG state dir). For now the binary is a banner-and-
 * exit stub so the CMake target exists and CI exercises the build path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IRON_VERSION_STRING
#define IRON_VERSION_STRING "0.0.0"
#endif
#ifndef IRON_GIT_HASH
#define IRON_GIT_HASH "unknown"
#endif
#ifndef IRON_BUILD_DATE
#define IRON_BUILD_DATE "unknown"
#endif
#ifndef IRON_BINARY_NAME
#define IRON_BINARY_NAME "ironls"
#endif

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 ||
                      strcmp(argv[1], "-v") == 0)) {
        printf("%s %s (%s, %s)\n",
               IRON_BINARY_NAME,
               IRON_VERSION_STRING,
               IRON_GIT_HASH,
               IRON_BUILD_DATE);
        return 0;
    }
    fprintf(stderr,
            "%s: iron language server (LSP 3.17)\n",
            IRON_BINARY_NAME);
    fprintf(stderr,
            "%s: Phase 2 skeleton -- transport/dispatch/store arrive in "
            "later plans\n",
            IRON_BINARY_NAME);
    return 0;
}
