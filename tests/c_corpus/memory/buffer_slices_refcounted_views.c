/*
 * title: Refcounted buffer with slice views
 * topic: memory
 * covers: shared backing buffer, sub-slices keep parent alive, slice of slice, zero-copy split, bytes-like API
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int rc;
    size_t cap;
    unsigned char *data;
} Backing;

typedef struct {
    Backing *b;
    size_t off, len;
} Bytes;

static int live_backing;
static int copies;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Bytes bytes_from(const void *src, size_t n) {
    Backing *b = malloc(sizeof *b);
    check(b != NULL, "alloc");
    b->data = malloc(n ? n : 1);
    check(b->data != NULL, "alloc");
    memcpy(b->data, src, n);
    b->cap = n;
    b->rc = 1;
    live_backing++;
    copies++;
    Bytes r = {b, 0, n};
    return r;
}

static void bytes_drop(Bytes *x) {
    if (x->b && --x->b->rc == 0) {
        memset(x->b->data, 0xEE, x->b->cap);
        free(x->b->data);
        free(x->b);
        live_backing--;
    }
    x->b = NULL;
    x->len = 0;
}

/* Zero-copy sub-slice: shares the backing store and bumps its refcount. */
static Bytes bytes_slice(Bytes x, size_t from, size_t to) {
    check(from <= to && to <= x.len, "slice range");
    x.b->rc++;
    Bytes r = {x.b, x.off + from, to - from};
    return r;
}

/* Split at n: two slices, both retained; the original is consumed by the caller separately. */
static void bytes_split(Bytes x, size_t n, Bytes *head, Bytes *tail) {
    *head = bytes_slice(x, 0, n);
    *tail = bytes_slice(x, n, x.len);
}

static const unsigned char *bytes_ptr(Bytes x) { return x.b->data + x.off; }

static Bytes bytes_concat(Bytes a, Bytes c) {
    unsigned char *tmp = malloc(a.len + c.len + 1);
    check(tmp != NULL, "alloc");
    memcpy(tmp, bytes_ptr(a), a.len);
    memcpy(tmp + a.len, bytes_ptr(c), c.len);
    Bytes r = bytes_from(tmp, a.len + c.len);
    free(tmp);
    return r;
}

static void show(const char *tag, Bytes x) {
    printf("%-8s off=%zu len=%zu rc=%d \"%.*s\"\n", tag, x.off, x.len, x.b->rc, (int)x.len,
           (const char *)bytes_ptr(x));
}

int main(void) {
    const char *msg = "GET /index.html HTTP/1.1";
    Bytes line = bytes_from(msg, strlen(msg));
    Bytes method, rest;
    bytes_split(line, 3, &method, &rest);
    Bytes path_and_ver = bytes_slice(rest, 1, rest.len);
    Bytes path, ver;
    size_t sp = 0;
    while (bytes_ptr(path_and_ver)[sp] != ' ')
        sp++;
    bytes_split(path_and_ver, sp, &path, &ver);
    Bytes version = bytes_slice(ver, 1, ver.len);

    show("line", line);
    show("method", method);
    show("path", path);
    show("version", version);
    printf("copies so far: %d, backing live: %d\n", copies, live_backing);
    check(copies == 1 && live_backing == 1, "zero copy");

    /* drop the whole line: the slices keep the buffer alive */
    bytes_drop(&line);
    bytes_drop(&rest);
    bytes_drop(&path_and_ver);
    bytes_drop(&ver);
    printf("after dropping parents: live=%d, path rc=%d, path=\"%.*s\"\n", live_backing, path.b->rc,
           (int)path.len, (const char *)bytes_ptr(path));

    Bytes joined = bytes_concat(method, path);
    show("joined", joined);
    printf("copies now: %d, backing live: %d\n", copies, live_backing);

    bytes_drop(&method);
    bytes_drop(&version);
    printf("live after dropping method,version: %d (path still holds original)\n", live_backing);
    check(live_backing == 2, "path + joined");
    bytes_drop(&path);
    printf("live after dropping path: %d\n", live_backing);
    bytes_drop(&joined);
    check(live_backing == 0, "leak");
    printf("final live: %d\n", live_backing);
    return 0;
}
