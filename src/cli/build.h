#ifndef IRON_CLI_BUILD_H
#define IRON_CLI_BUILD_H

#include <stdbool.h>

typedef struct {
    bool        verbose;
    bool        debug_build;
    bool        run_after;        /* true for "iron run" */
    const char **run_args;        /* args after -- */
    int          run_arg_count;
    bool        use_raylib;       /* true when iron.toml has [dependencies] raylib = true */
    bool        force_comptime;   /* --force-comptime flag: skip comptime cache */
} IronBuildOpts;

/* Build a .iron source file to a native binary.
 * source_path: path to the .iron file
 * output_path: desired output binary path, or NULL to derive from source name
 * opts: build options
 * Returns 0 on success, non-zero on error.
 */
int iron_build(const char *source_path, const char *output_path,
               IronBuildOpts opts);

#endif /* IRON_CLI_BUILD_H */
