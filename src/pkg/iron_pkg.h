#ifndef IRON_PKG_H
#define IRON_PKG_H

/* Shared helpers exposed from main.c for use by init.c and pkg_build.c */

/* Returns malloc'd path to the ironc binary (caller must free). */
char *find_ironc(void);

/* Forward the current invocation to ironc verbatim.
 * Returns ironc's exit code. */
int forward_to_ironc(int argc, char **argv);

/* Spawn an arbitrary program with the given argv (NULL-terminated).
 * prog is the path/name of the binary.
 * Returns the child exit code, or 1 on spawn failure. */
int spawn_and_wait(const char *prog, char *const argv[]);

#endif /* IRON_PKG_H */
