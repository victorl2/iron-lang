#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "cli/build.h"
#include "cli/check.h"
#include "cli/fmt.h"
#include "cli/test_runner.h"
#include "cli/version.h"

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
    fprintf(stderr, "  migrate Migrate .iron source from v2 to v3 grammar\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --version         Print version and exit\n");
    fprintf(stderr, "  --target=<t>      Build target: native (default) or web\n");
    fprintf(stderr, "  --release         Optimize build (native -O2, web -Oz -flto)\n");
    fprintf(stderr, "  --emit-archive    Compile to static archive (lib<name>.a) instead of executable (Phase 94 LIB)\n");
    fprintf(stderr, "  --verbose         Show generated C code\n");
    fprintf(stderr, "  --debug-build     Keep .iron-build/ directory\n");
    fprintf(stderr, "  --force-comptime  Skip comptime evaluation cache\n");
    fprintf(stderr, "  --dump-ir-passes  Print IR after each optimization pass\n");
    fprintf(stderr, "  --no-optimize     Skip optimization passes (for A/B comparison)\n");
    fprintf(stderr, "  --warn-fusion-break  Show where fusion chains are broken by non-fusible calls\n");
    fprintf(stderr, "  --report-compression Show which fields were narrowed for value range compression\n");
    fprintf(stderr, "  --no-strict-v3       Disable v3.0 breaking-change rejections (for debugging v2 syntax; default is ON)\n");
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
    bool warn_fusion_break = false;
    bool report_compression = false;
    bool strict_v3 = true;
    IronBuildTarget target = IRON_TARGET_NATIVE;
    bool release = false;
    bool emit_archive = false;
    const char *pkg_name_arg = NULL;
    const char *pkg_version_arg = NULL;
    const char *source_file = NULL;
    const char *output_file = NULL;
    const char **run_args = NULL;
    int run_arg_count = 0;
    /* Phase 94 LIB-03: collect -L<dir> / -l<name> argv entries for forwarding
     * to clang's link line. The pkg_build layer emits these per local-path
     * dep; main.c stores them on IronBuildOpts.extra_link_flags so build.c
     * can append them alongside the existing -lm. */
    const char *extra_link_flags_buf[32];
    int extra_link_flag_count = 0;

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
        } else if (strcmp(argv[i], "--warn-fusion-break") == 0) {
            warn_fusion_break = true;
        } else if (strcmp(argv[i], "--report-compression") == 0) {
            report_compression = true;
        } else if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            } else {
                fprintf(stderr, "%s: --output requires a path argument\n", IRON_BINARY_NAME);
                return 1;
            }
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            const char *val = argv[i] + 9;
            if (strcmp(val, "web") == 0) {
                target = IRON_TARGET_WEB;
            } else if (strcmp(val, "native") == 0) {
                target = IRON_TARGET_NATIVE;
            } else {
                fprintf(stderr, "error: unknown target '%s'. valid targets: web, native\n", val);
                return 1;
            }
        } else if (strcmp(argv[i], "--target") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --target requires a value (web or native)\n");
                return 1;
            }
            const char *val = argv[++i];
            if (strcmp(val, "web") == 0) {
                target = IRON_TARGET_WEB;
            } else if (strcmp(val, "native") == 0) {
                target = IRON_TARGET_NATIVE;
            } else {
                fprintf(stderr, "error: unknown target '%s'. valid targets: web, native\n", val);
                return 1;
            }
        } else if (strcmp(argv[i], "--release") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--emit-archive") == 0) {
            emit_archive = true;
        } else if (strcmp(argv[i], "--pkg-name") == 0) {
            if (i + 1 < argc) {
                pkg_name_arg = argv[++i];
            } else {
                fprintf(stderr, "%s: --pkg-name requires a value\n", IRON_BINARY_NAME);
                return 1;
            }
        } else if (strcmp(argv[i], "--pkg-version") == 0) {
            if (i + 1 < argc) {
                pkg_version_arg = argv[++i];
            } else {
                fprintf(stderr, "%s: --pkg-version requires a value\n", IRON_BINARY_NAME);
                return 1;
            }
        } else if (strcmp(argv[i], "--strict-v3") == 0) {
            strict_v3 = true;
        } else if (strcmp(argv[i], "--no-strict-v3") == 0) {
            strict_v3 = false;
        } else if ((strncmp(argv[i], "-L", 2) == 0 && argv[i][2] != '\0') ||
                   (strncmp(argv[i], "-l", 2) == 0 && argv[i][2] != '\0')) {
            /* Phase 94 LIB-03: collect -L<dir> / -l<name> for the link line.
             * Reject the bare "-L" / "-l" forms (no inline value) — the
             * pkg_build layer emits inline form only. */
            if (extra_link_flag_count <
                (int)(sizeof(extra_link_flags_buf) / sizeof(extra_link_flags_buf[0]))) {
                extra_link_flags_buf[extra_link_flag_count++] = argv[i];
            } else {
                fprintf(stderr,
                        "%s: too many -L/-l link flags (max %zu)\n",
                        IRON_BINARY_NAME,
                        sizeof(extra_link_flags_buf) / sizeof(extra_link_flags_buf[0]));
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
            .no_optimize    = no_optimize,
            .warn_fusion_break = warn_fusion_break,
            .report_compression = report_compression,
            .target         = target,
            .release        = release,
            .strict_v3      = strict_v3,
            .emit_archive   = emit_archive,
            .pkg_name       = pkg_name_arg,
            .pkg_version    = pkg_version_arg,
            .extra_link_flags = extra_link_flag_count > 0 ? extra_link_flags_buf : NULL,
            .extra_link_flag_count = extra_link_flag_count
        };
        return iron_build(source_file, output_file, opts);
    }

    /* Phase 96 RUN-03 (reserved, NOT implemented in v3.2):
     *   --keep-binary  reserved to suppress the atexit unlink of the
     *                  ${TMPDIR}/iron-run-XXXXXX tempfile produced by the
     *                  direct-source `iron run foo.iron` path.
     *   -o <path>      reserved as an output-path override for `iron run`.
     * Both flags are documented in `iron run --help` (Phase 97 HELP-03 scope).
     * Implementing them in v3.2 was descoped: the cwd-clean default (mkstemp
     * + atexit in iron_build) covers the primary issue (#53); a deliberate
     * keep-binary flag belongs in a later phase alongside the broader CLI
     * help registry work. */
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
            .no_optimize    = no_optimize,
            .warn_fusion_break = warn_fusion_break,
            .report_compression = report_compression,
            .target         = target,
            .release        = release,
            .strict_v3      = strict_v3,
            .emit_archive   = false,
            .pkg_name       = NULL,
            .pkg_version    = NULL,
            .extra_link_flags = extra_link_flag_count > 0 ? extra_link_flags_buf : NULL,
            .extra_link_flag_count = extra_link_flag_count
        };
        return iron_build(source_file, output_file, opts);
    }

    if (strcmp(cmd, "check") == 0) {
        if (!source_file) {
            fprintf(stderr, "%s check: missing source file\n", IRON_BINARY_NAME);
            return 1;
        }
        return iron_check(source_file, verbose, strict_v3);
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

    if (strcmp(cmd, "migrate") == 0) {
        /* ironc migrate --from v2 --to v3 <path> */
        const char *from_ver = NULL;
        const char *to_ver = NULL;
        const char *target_path = NULL;

        for (int j = 2; j < argc; j++) {
            if (strcmp(argv[j], "--from") == 0 && j + 1 < argc) {
                from_ver = argv[++j];
            } else if (strcmp(argv[j], "--to") == 0 && j + 1 < argc) {
                to_ver = argv[++j];
            } else if (argv[j][0] != '-') {
                target_path = argv[j];
            }
        }

        if (!from_ver || !to_ver || !target_path) {
            fprintf(stderr, "%s migrate: usage: ironc migrate --from v2 --to v3 <path>\n", IRON_BINARY_NAME);
            return 1;
        }

        /* Locate migrate script relative to cwd (scripts/ directory) */
        char script_path[1024];
        snprintf(script_path, sizeof(script_path), "scripts/migrate_v2_to_v3.py");

        const char *exec_argv[] = {
            "python3", script_path,
            "--from", from_ver,
            "--to", to_ver,
            target_path,
            NULL
        };
        execvp("python3", (char *const *)exec_argv);
        /* execvp only returns on error */
        perror("ironc migrate: failed to exec python3");
        return 1;
    }

    fprintf(stderr, "%s: unknown command '%s'\n", IRON_BINARY_NAME, cmd);
    print_usage();
    return 1;
}
