#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cli/build.h"
#include "cli/check.h"
#include "cli/fmt.h"
#include "cli/test_runner.h"

#define IRON_VERSION "0.0.1-alpha"

static void print_version(void) {
    printf("iron %s\n", IRON_VERSION);
}

static void print_usage(void) {
    fprintf(stderr, "Usage: iron <command> [options] <file>\n\n");
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
    const char *source_file = NULL;
    const char **run_args = NULL;
    int run_arg_count = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--debug-build") == 0) {
            debug_build = true;
        } else if (strcmp(argv[i], "--force-comptime") == 0) {
            force_comptime = true;
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
            fprintf(stderr, "iron build: missing source file\n");
            return 1;
        }
        IronBuildOpts opts = {
            .verbose        = verbose,
            .debug_build    = debug_build,
            .run_after      = false,
            .run_args       = NULL,
            .run_arg_count  = 0,
            .use_raylib     = false,
            .force_comptime = force_comptime
        };
        return iron_build(source_file, NULL, opts);
    }

    if (strcmp(cmd, "run") == 0) {
        if (!source_file) {
            fprintf(stderr, "iron run: missing source file\n");
            return 1;
        }
        IronBuildOpts opts = {
            .verbose        = verbose,
            .debug_build    = debug_build,
            .run_after      = true,
            .run_args       = run_args,
            .run_arg_count  = run_arg_count,
            .use_raylib     = false,
            .force_comptime = force_comptime
        };
        return iron_build(source_file, NULL, opts);
    }

    if (strcmp(cmd, "check") == 0) {
        if (!source_file) {
            fprintf(stderr, "iron check: missing source file\n");
            return 1;
        }
        return iron_check(source_file, verbose);
    }

    if (strcmp(cmd, "fmt") == 0) {
        if (!source_file) {
            fprintf(stderr, "iron fmt: missing file argument\nUsage: iron fmt <file.iron>\n");
            return 1;
        }
        return iron_fmt(source_file);
    }

    if (strcmp(cmd, "test") == 0) {
        return iron_test(source_file ? source_file : ".");
    }

    fprintf(stderr, "iron: unknown command '%s'\n", cmd);
    print_usage();
    return 1;
}
