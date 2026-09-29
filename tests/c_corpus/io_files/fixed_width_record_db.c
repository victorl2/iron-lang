/*
 * title: Fixed-width text record database
 * topic: io_files
 * covers: r+ in-place updates, record numbering by offset, tombstones, lookup scan, compaction into new file
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* one record: "IIII NNNNNNNNNNNN BBBBBBBB S\n" = 4+1+12+1+8+1+1+1 bytes */
enum { RECLEN = 29, NAMELEN = 12 };
#define DB "people.db"

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    int id;
    char name[NAMELEN + 1];
    long balance;
    int live;
} Rec;

static void fmt(char *out, const Rec *r) {
    int n = snprintf(out, RECLEN + 1, "%04d %-*.*s %08ld %c\n", r->id, NAMELEN, NAMELEN, r->name,
                     r->balance, r->live ? 'A' : 'D');
    check(n == RECLEN, "record width");
}

static int parse(const char *line, Rec *r) {
    /* columns: id 0-3, name 5-16, balance 18-25, status 27 */
    if (strlen(line) < RECLEN - 1 || line[4] != ' ' || line[17] != ' ' || line[26] != ' ')
        return 0;
    r->id = atoi(line);
    memcpy(r->name, line + 5, NAMELEN);
    r->name[NAMELEN] = 0;
    for (int i = NAMELEN - 1; i >= 0 && r->name[i] == ' '; i--)
        r->name[i] = 0;
    r->balance = atol(line + 18);
    r->live = line[27] == 'A';
    return 1;
}

static long add(FILE *f, int id, const char *name, long bal) {
    Rec r = {id, "", bal, 1};
    snprintf(r.name, sizeof r.name, "%s", name);
    char buf[RECLEN + 1];
    fmt(buf, &r);
    fseek(f, 0, SEEK_END);
    long off = ftell(f);
    check(fwrite(buf, 1, RECLEN, f) == RECLEN, "add");
    return off / RECLEN;
}

static int get(FILE *f, long idx, Rec *r) {
    char buf[RECLEN + 1];
    if (fseek(f, idx * RECLEN, SEEK_SET) != 0)
        return 0;
    if (fread(buf, 1, RECLEN, f) != RECLEN)
        return 0;
    buf[RECLEN] = 0;
    return parse(buf, r);
}

static int put(FILE *f, long idx, const Rec *r) {
    char buf[RECLEN + 1];
    fmt(buf, r);
    check(fseek(f, idx * RECLEN, SEEK_SET) == 0, "put seek");
    return fwrite(buf, 1, RECLEN, f) == RECLEN;
}

static long find(FILE *f, int id) {
    Rec r;
    for (long i = 0; get(f, i, &r); i++)
        if (r.live && r.id == id)
            return i;
    return -1;
}

static void list(FILE *f, const char *title) {
    Rec r;
    printf("%s\n", title);
    for (long i = 0; get(f, i, &r); i++)
        printf("  #%ld %04d %-12s %8ld %s\n", i, r.id, r.name, r.balance, r.live ? "live" : "deleted");
}

int main(void) {
    FILE *f = fopen(DB, "w+");
    check(f != NULL, "create");
    const char *names[] = {"ada", "grace", "linus", "dennis", "ken", "margaret", "a very long name"};
    for (int i = 0; i < 7; i++) {
        long idx = add(f, 100 + i * 10, names[i], 1000L * (i + 1) + i);
        check(idx == i, "record index");
    }
    fflush(f);
    list(f, "after inserts:");

    long at = find(f, 130);
    Rec r;
    check(at == 3 && get(f, at, &r), "find 130");
    r.balance += 250;
    check(put(f, at, &r), "update");

    at = find(f, 110);
    check(get(f, at, &r), "get 110");
    r.live = 0;
    check(put(f, at, &r), "delete");
    printf("find 110 after delete: %ld\n", find(f, 110));
    printf("find 999: %ld\n", find(f, 999));
    list(f, "after update and delete:");

    /* file size is always a multiple of the record length */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    printf("size %ld = %ld records x %d\n", size, size / RECLEN, RECLEN);
    check(size == 7L * RECLEN, "size");
    fclose(f);

    /* compact: copy live records to a new file, then swap */
    FILE *in = fopen(DB, "r");
    FILE *out = fopen("compact.db", "w");
    check(in && out, "compact open");
    char buf[RECLEN + 1];
    int kept = 0, dropped = 0;
    long total = 0;
    while (fread(buf, 1, RECLEN, in) == RECLEN) {
        buf[RECLEN] = 0;
        check(parse(buf, &r), "parse in compact");
        if (r.live) {
            check(fwrite(buf, 1, RECLEN, out) == RECLEN, "compact write");
            kept++;
            total += r.balance;
        } else
            dropped++;
    }
    fclose(in);
    check(fclose(out) == 0, "compact close");
    check(rename("compact.db", DB) == 0, "swap");
    printf("compacted: kept %d dropped %d, live balance total %ld\n", kept, dropped, total);

    f = fopen(DB, "r+");
    check(f != NULL, "reopen");
    list(f, "after compaction:");
    fseek(f, 0, SEEK_END);
    check(ftell(f) == 6L * RECLEN, "compact size");
    /* out-of-range read fails cleanly */
    printf("record 6 readable: %d\n", get(f, 6, &r));
    check(!get(f, 6, &r), "out of range");
    fclose(f);
    remove(DB);
    return 0;
}
