#ifndef IRON_PKG_COLOR_H
#define IRON_PKG_COLOR_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* Iron-branded orange: 256-color code 208 */
#define IRON_COLOR_ORANGE  "\x1b[38;5;208m"
#define IRON_COLOR_GREEN   "\x1b[32m"
#define IRON_COLOR_RED     "\x1b[31m"
#define IRON_COLOR_CYAN    "\x1b[36m"
#define IRON_COLOR_BOLD    "\x1b[1m"
#define IRON_COLOR_DIM     "\x1b[2m"
#define IRON_COLOR_RESET   "\x1b[0m"

/* Call once at startup. Returns true if colors are enabled. */
static inline bool iron_color_init(void) {
    if (getenv("NO_COLOR")) return false;
    if (getenv("FORCE_COLOR")) return true;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    return true;
#else
    return isatty(STDOUT_FILENO) ? true : false;
#endif
}

/*
 * Print Cargo-style status line:
 *   "   Compiling hello v0.1.0"
 *   "    Finished dev [unoptimized] in 0.42s"
 * Verb is right-aligned to 12 chars. With color: verb is orange+bold.
 * Output goes to stderr (Cargo convention — status to stderr, program output to stdout).
 */
static inline void iron_print_status(bool color, const char *verb,
                                      const char *detail) {
    if (color) {
        fprintf(stderr, "%s%s%12s%s %s\n",
               IRON_COLOR_BOLD, IRON_COLOR_ORANGE,
               verb, IRON_COLOR_RESET, detail);
    } else {
        fprintf(stderr, "%12s %s\n", verb, detail);
    }
    fflush(stderr);
}

/* Print error prefix: "error:" in red+bold */
static inline void iron_print_error(bool color, const char *msg) {
    if (color) {
        fprintf(stderr, "%s%serror:%s %s\n",
                IRON_COLOR_BOLD, IRON_COLOR_RED,
                IRON_COLOR_RESET, msg);
    } else {
        fprintf(stderr, "error: %s\n", msg);
    }
}

#endif /* IRON_PKG_COLOR_H */
