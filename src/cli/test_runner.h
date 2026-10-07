#ifndef IRON_CLI_TEST_RUNNER_H
#define IRON_CLI_TEST_RUNNER_H

/* Discover test_*.iron files in dir_path and run each as a compiled binary.
 * dir_path: directory to search (NULL means current directory ".")
 * Returns 0 if all tests passed, 1 if any failed. */
#include <stdbool.h>

int iron_test(const char *dir_path);

/* Whether the file declares `func main(` at the start of a line. */
bool iron_file_defines_main(const char *path);

/* Builds `source` as a test binary (`test "name" { ... }` blocks), runs each
 * test whose name contains `filter` (NULL: all) in its own process, several
 * at a time, and reports per test. Returns 0 when all pass. */
int iron_test_file(const char *source, const char *filter);

#endif /* IRON_CLI_TEST_RUNNER_H */
