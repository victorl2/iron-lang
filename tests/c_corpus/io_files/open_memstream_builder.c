/*
 * title: open_memstream growing output buffer
 * topic: io_files
 * covers: open_memstream, fflush size update, fseek in memstream, table rendering to memory, free after close
 * deps: posix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    const char *name;
    int qty;
    double price;
} Item;

int main(void) {
    char *buf = NULL;
    size_t size = 0;
    FILE *m = open_memstream(&buf, &size);
    check(m != NULL, "open_memstream");

    fprintf(m, "hello");
    fflush(m);
    printf("after first write: size %zu content \"%s\"\n", size, buf);
    check(size == 5 && strcmp(buf, "hello") == 0, "first");

    fprintf(m, ", %s!", "world");
    fflush(m);
    printf("after second write: size %zu content \"%s\"\n", size, buf);
    check(size == 13 && strcmp(buf, "hello, world!") == 0, "second");

    /* seeking back overwrites; size reflects the highest position at flush */
    fseek(m, 7, SEEK_SET);
    fputs("WORLD", m);
    fflush(m);
    printf("after overwrite: size %zu content \"%s\"\n", size, buf);
    check(strcmp(buf, "hello, WORLD!") == 0, "overwrite");
    fclose(m);
    free(buf);

    /* build a report of a size that forces many reallocations inside the stream */
    buf = NULL;
    size = 0;
    m = open_memstream(&buf, &size);
    check(m != NULL, "second stream");
    Item items[] = {{"bolt", 120, 0.25}, {"washer", 4000, 0.05}, {"gear", 12, 14.5}, {"chain", 3, 89.99}};
    double total = 0;
    fprintf(m, "%-8s %6s %9s %10s\n", "item", "qty", "price", "amount");
    for (int i = 0; i < 4; i++) {
        double amt = items[i].qty * items[i].price;
        total += amt;
        fprintf(m, "%-8s %6d %9.2f %10.2f\n", items[i].name, items[i].qty, items[i].price, amt);
    }
    fprintf(m, "%-8s %6s %9s %10.2f\n", "total", "", "", total);
    for (int i = 0; i < 300; i++)
        fprintf(m, "filler line %03d\n", i);
    check(fclose(m) == 0, "close");
    printf("report is %zu bytes\n", size);
    check(strlen(buf) == size, "size matches strlen");

    /* print just the table part (first 6 lines) */
    int lines = 0;
    for (const char *p = buf; *p && lines < 6; lines++) {
        const char *e = strchr(p, '\n');
        printf("| %.*s\n", (int)(e - p), p);
        p = e + 1;
    }
    check(total > 673.9 && total < 674.0, "total");

    /* count lines by re-reading the buffer through fmemopen */
    FILE *rd = fmemopen(buf, size, "r");
    check(rd != NULL, "fmemopen");
    int n = 0;
    char line[64];
    while (fgets(line, sizeof line, rd))
        n++;
    fclose(rd);
    printf("lines in report: %d\n", n);
    check(n == 306, "lines");
    free(buf);

    /* empty stream: buffer is a valid empty string after close */
    m = open_memstream(&buf, &size);
    check(m != NULL, "empty");
    fclose(m);
    printf("empty stream: size %zu, string \"%s\"\n", size, buf);
    check(size == 0 && buf[0] == 0, "empty");
    free(buf);
    return 0;
}
