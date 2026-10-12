/*
 * title: freopen to switch files and modes
 * topic: io_files
 * covers: freopen on a named stream, write to read switch, redirecting stdout to a file and back
 * deps: posix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void dump(const char *name) {
    FILE *f = fopen(name, "r");
    check(f != NULL, "dump open");
    char line[64];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        printf("  %s[%d] %s\n", name, ++n, line);
    }
    fclose(f);
}

int main(void) {
    /* one FILE object, three different files and modes in turn */
    FILE *f = fopen("one.txt", "w");
    check(f != NULL, "open one");
    fputs("first file\n", f);

    f = freopen("two.txt", "w", f);
    check(f != NULL, "freopen two");
    fputs("second file, line 1\nsecond file, line 2\n", f);

    f = freopen("one.txt", "a", f);
    check(f != NULL, "freopen one append");
    fputs("appended to first\n", f);

    f = freopen("two.txt", "r", f);
    check(f != NULL, "freopen two read");
    char line[64];
    check(fgets(line, sizeof line, f) != NULL, "read two");
    line[strcspn(line, "\n")] = 0;
    printf("read via reopened stream: \"%s\"\n", line);
    check(fputc('x', f) == EOF, "cannot write to read stream");
    clearerr(f);
    fclose(f);

    dump("one.txt");
    dump("two.txt");

    /* a failed freopen is not exercised: glibc leaves the FILE allocated but unusable, so a
     * portable program cannot release it */

    /* redirect stdout to a file, then restore it through a saved descriptor */
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    check(saved >= 0, "dup");
    check(freopen("captured.txt", "w", stdout) != NULL, "freopen stdout");
    printf("this line goes to the file\n");
    printf("so does %s\n", "this one");
    fflush(stdout);
    check(dup2(saved, STDOUT_FILENO) >= 0, "restore stdout");
    close(saved);
    clearerr(stdout);
    puts("stdout is back");
    dump("captured.txt");

    /* the captured file has exactly two lines and 44 bytes */
    f = fopen("captured.txt", "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    printf("captured %ld bytes\n", sz);
    check(sz == 44, "captured size");

    remove("one.txt");
    remove("two.txt");
    remove("captured.txt");
    return 0;
}
