#ifndef IRON_PROJECT_BUILD_H
#define IRON_PROJECT_BUILD_H

/* Handle project-mode commands: build, run, check, test, fmt.
 * cmd is one of "build", "run", "check", "test", "fmt".
 * Returns exit code. */
int cmd_project(const char *cmd, int argc, char **argv);

/* `iron debug file.iron [--gdb|--lldb] [-- args]`: build the file with
 * --debug into a temporary directory and debug it. */
int iron_debug_file(int argc, char **argv);

#endif /* IRON_PROJECT_BUILD_H */
