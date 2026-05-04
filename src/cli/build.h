#ifndef IRON_CLI_BUILD_H
#define IRON_CLI_BUILD_H

#include <stdbool.h>

typedef enum {
    IRON_TARGET_NATIVE = 0,   /* Default: native clang build (current behavior). */
    IRON_TARGET_WEB    = 1    /* --target=web: dispatch to build_web.c. */
} IronBuildTarget;

typedef struct {
    bool        verbose;
    bool        debug_build;
    bool        run_after;        /* true for "iron run" */
    const char **run_args;        /* args after -- */
    int          run_arg_count;
    bool        use_raylib;       /* true when iron.toml has [dependencies] raylib = true */
    bool        force_comptime;   /* --force-comptime flag: skip comptime cache */
    bool        dump_ir_passes;   /* --dump-ir-passes: print IR after each opt pass */
    bool        no_optimize;      /* --no-optimize: skip copy-prop/const-fold/DCE */
    bool        warn_fusion_break; /* --warn-fusion-break: emit diagnostics at fusion chain break points */
    bool        report_compression; /* --report-compression: show which fields were narrowed */
    IronBuildTarget target;   /* --target=native|web. Default IRON_TARGET_NATIVE. */
    bool            release;  /* --release flag. Native: clang -O2. Web: Phase 7 consumes for -Oz -flto -sASSERTIONS=0. */
    bool            strict_v3; /* --strict-v3: Phase 88 BREAK gate (enables E0260..E0264 rejections) */
    bool            emit_archive;  /* Phase 94 LIB-01: produce target/lib<name>.a, not exe.
                                    * Pipeline: clang -c -o <obj>.o <c_file>, then llvm-ar (or ar
                                    * fallback) rcs <output> <obj>.o, then unlink <obj>.o on success.
                                    * Mutually exclusive with target == IRON_TARGET_WEB (web archives
                                    * are out of v3.2 scope). */
    const char     *pkg_name;       /* Phase 94 LIB-02: for .iron-stub header. NULL = derive from output path. */
    const char     *pkg_version;    /* Phase 94 LIB-02: for .iron-stub header. NULL = "0.0.0". */
    /* Phase 94 LIB-03: extra link flags forwarded to clang link line (e.g.
     * -L<path-dep>/target -l<dep-name> for each local-path dependency).
     * Each entry is appended verbatim to the clang argv just before the
     * existing -lm. NULL / 0 when no path-deps are present. main.c collects
     * any argv entry starting with "-L" or "-l" into this array. */
    const char    **extra_link_flags;
    int             extra_link_flag_count;
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
