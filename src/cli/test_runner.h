#ifndef IRON_CLI_TEST_RUNNER_H
#define IRON_CLI_TEST_RUNNER_H

/* Discover test_*.iron files in dir_path and run each as a compiled binary.
 * dir_path: directory to search (NULL means current directory ".")
 * Returns 0 if all tests passed, 1 if any failed. */
int iron_test(const char *dir_path);

#endif /* IRON_CLI_TEST_RUNNER_H */
