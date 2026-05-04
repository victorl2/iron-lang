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
#include "cli/help_registry.h"

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

/*
 * print_usage: forwards to the central help registry. Phase 97 HELP-01
 * routes both error paths (no-args, unknown-command) and the top-level
 * --help request through iron_help_print_all on stdout. The exit code at
 * the call sites stays unchanged: --help requests exit 0; usage errors
 * (no-args, unknown-command) exit 1. Both print the same help text.
 */
static void print_usage(void) {
    iron_help_print_all("ironc", stdout);
}

/*
 * argv_contains_help: scan argv[start..argc) for --help or -h.
 * Returns 1 if found anywhere, 0 otherwise. Mirrors the helper in
 * src/pkg/main.c — kept inline rather than refactored into a shared
 * header because a 6-line helper does not justify a header round-trip.
 */
static int argv_contains_help(int argc, char **argv, int start) {
    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            return 1;
        }
    }
    return 0;
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

    /* Top-level --help / -h */
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        iron_help_print_all("ironc", stdout);
        return 0;
    }

    /*
     * Phase 97 HELP-01 / HELP-06: pre-dispatch --help scan for ironc.
     * Mirrors the iron-side scan in src/pkg/main.c. Subcommands recognized
     * by ironc: build, run, check, fmt, test, migrate (no `init` — that's
     * an iron-only command). Fires BEFORE the global-flag-parsing argv
     * loop below so iron_build / iron_check / iron_fmt / iron_test /
     * migrate handlers never run for --help requests.
     */
    {
        static const char *KNOWN_SUBS[] = {
            "build", "run", "check", "fmt", "test", "migrate", NULL
        };
        int is_known_sub = 0;
        for (int i = 0; KNOWN_SUBS[i]; i++) {
            if (strcmp(cmd, KNOWN_SUBS[i]) == 0) { is_known_sub = 1; break; }
        }
        if (is_known_sub && argv_contains_help(argc, argv, 2)) {
            iron_help_print_subcommand("ironc", cmd, stdout);
            return 0;
        }
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
        /* Phase 5 Plan 05-01 (D-15): scan argv for --check flag.
         * Supported invocations:
         *   iron fmt <file>
         *   iron fmt --check <file>
         *   iron fmt <file> --check
         * Multi-file --check deferred to v1.x. */
        bool check_mode = false;
        const char *file_arg = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--check") == 0) {
                check_mode = true;
            } else if (strncmp(argv[i], "--", 2) == 0) {
                /* Skip unrelated global flags already consumed above. */
                continue;
            } else if (file_arg == NULL) {
                file_arg = argv[i];
            }
        }
        if (!file_arg) {
            fprintf(stderr, "%s fmt: missing file argument\nUsage: %s fmt [--check] <file.iron>\n",
                    IRON_BINARY_NAME, IRON_BINARY_NAME);
            return 1;
        }
        return check_mode ? iron_fmt_check(file_arg) : iron_fmt(file_arg);
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
