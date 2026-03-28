#ifndef IRON_PKG_BUILD_H
#define IRON_PKG_BUILD_H

/* Handle package-mode commands: build, run, check, test, fmt.
 * cmd is one of "build", "run", "check", "test", "fmt".
 * Returns exit code. */
int cmd_package(const char *cmd, int argc, char **argv);

#endif /* IRON_PKG_BUILD_H */
