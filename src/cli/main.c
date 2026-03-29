#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cli/build.h"
#include "cli/check.h"
#include "cli/fmt.h"
#include "cli/test_runner.h"

#ifndef IRON_VERSION_STRING
#define IRON_VERSION_STRING "0.1.1"
#endif
#ifndef IRON_GIT_HASH
#define IRON_GIT_HASH "unknown"
#endif
#ifndef IRON_BUILD_DATE
#define IRON_BUILD_DATE "unknown"
#endif
#ifndef IRON_BINARY_NAME
#define IRON_BINARY_NAME "ironc"
#endif

static void print_version(void) {
    printf("%s %s (%s %s)\n", IRON_BINARY_NAME, IRON_VERSION_STRING, IRON_GIT_HASH, IRON_BUILD_DATE);
}

static void print_usage(void) {
    fprintf(stderr, "Usage: %s <command> [options] <file>\n\n", IRON_BINARY_NAME);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  build   Compile .iron file to native binary\n");
    fprintf(stderr, "  run     Compile and execute .iron file\n");
    fprintf(stderr, "  check   Type-check without compiling\n");
    fprintf(stderr, "  fmt     Format Iron source code\n");
    fprintf(stderr, "  test    Discover and run Iron tests\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --version         Print version and exit\n");
    fprintf(stderr, "  --verbose         Show generated C code\n");
    fprintf(stderr, "  --debug-build     Keep .iron-build/ directory\n");
    fprintf(stderr, "  --force-comptime  Skip comptime evaluation cache\n");
    fprintf(stderr, "  --dump-ir-passes  Print IR after each optimization pass\n");
    fprintf(stderr, "  --no-optimize     Skip optimization passes (for A/B comparison)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        print_version();
        return 0;
    }

    /* Parse global flags */
    bool verbose = false;
    bool debug_build = false;
    bool force_comptime = false;
    bool dump_ir_passes = false;
    bool no_optimize = false;
    const char *source_file = NULL;
    const char *output_file = NULL;
    const char **run_args = NULL;
    int run_arg_count = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--debug-build") == 0) {
            debug_build = true;
        } else if (strcmp(argv[i], "--force-comptime") == 0) {
            force_comptime = true;
        } else if (strcmp(argv[i], "--dump-ir-passes") == 0) {
            dump_ir_passes = true;
        } else if (strcmp(argv[i], "--no-optimize") == 0) {
            no_optimize = true;
        } else if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            } else {
                fprintf(stderr, "%s: --output requires a path argument\n", IRON_BINARY_NAME);
                return 1;
            }
        } else if (strcmp(argv[i], "--") == 0) {
            /* Everything after -- is passed to the program (iron run) */
            run_args = (const char **)&argv[i + 1];
            run_arg_count = argc - i - 1;
            break;
        } else if (!source_file) {
            source_file = argv[i];
        }
    }

    if (strcmp(cmd, "build") == 0) {
        if (!source_file) {
            fprintf(stderr, "%s build: missing source file\n", IRON_BINARY_NAME);
            return 1;
        }
        IronBuildOpts opts = {
            .verbose        = verbose,
            .debug_build    = debug_build,
            .run_after      = false,
            .run_args       = NULL,
            .run_arg_count  = 0,
            .use_raylib     = false,
            .force_comptime = force_comptime,
            .dump_ir_passes = dump_ir_passes,
            .no_optimize    = no_optimize
        };
        return iron_build(source_file, output_file, opts);
    }

    if (strcmp(cmd, "run") == 0) {
        if (!source_file) {
            fprintf(stderr, "%s run: missing source file\n", IRON_BINARY_NAME);
            return 1;
        }
        IronBuildOpts opts = {
            .verbose        = verbose,
            .debug_build    = debug_build,
            .run_after      = true,
            .run_args       = run_args,
            .run_arg_count  = run_arg_count,
            .use_raylib     = false,
            .force_comptime = force_comptime,
            .dump_ir_passes = dump_ir_passes,
            .no_optimize    = no_optimize
        };
        return iron_build(source_file, output_file, opts);
    }

    if (strcmp(cmd, "check") == 0) {
        if (!source_file) {
            fprintf(stderr, "%s check: missing source file\n", IRON_BINARY_NAME);
            return 1;
        }
        return iron_check(source_file, verbose);
    }

    if (strcmp(cmd, "fmt") == 0) {
        if (!source_file) {
            fprintf(stderr, "%s fmt: missing file argument\nUsage: %s fmt <file.iron>\n", IRON_BINARY_NAME, IRON_BINARY_NAME);
            return 1;
        }
        return iron_fmt(source_file);
    }

    if (strcmp(cmd, "test") == 0) {
        return iron_test(source_file ? source_file : ".");
    }

    fprintf(stderr, "%s: unknown command '%s'\n", IRON_BINARY_NAME, cmd);
    print_usage();
    return 1;
}
