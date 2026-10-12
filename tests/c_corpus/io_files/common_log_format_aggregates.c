/*
 * title: Common Log Format parser with per-status and per-path aggregates
 * topic: io_files
 * covers: apache common log format, bracketed timestamps, quoted request, month names, epoch conversion, top-N, malformed line accounting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

unsigned char *rfile(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    if (!f) fail("open for read");
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap);
    if (!b) fail("oom");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) fail("oom");
        }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}


static uint32_t rng_s = 0x2545F491u;
uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}
/* growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;
void bput(Buf *b, const void *s, size_t k) {
    if (b->n + k > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->n + k) nc *= 2;
        b->p = realloc(b->p, nc);
        if (!b->p) fail("oom");
        b->cap = nc;
    }
    if (k) memcpy(b->p + b->n, s, k);
    b->n += k;
}
void bbyte(Buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

typedef struct {
    char host[40], ident[16], user[16], method[8], path[64], proto[12];
    int day, mon, year, hh, mm, ss, tz_min;
    int status;
    long bytes;      /* -1 for "-" */
    long epoch;      /* UTC seconds */
} Entry;

static const char *MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int parse_line(const char *s, Entry *e) {
    memset(e, 0, sizeof *e);
    int n = 0;
    /* host ident user */
    if (sscanf(s, "%39s %15s %15s [%n", e->host, e->ident, e->user, &n) < 3 || n == 0) return 0;
    const char *p = s + n;
    char mon[4];
    int sign = 1, tzh = 0, tzm = 0, k = 0;
    char sg;
    if (sscanf(p, "%d/%3[A-Za-z]/%d:%d:%d:%d %c%2d%2d]%n", &e->day, mon, &e->year, &e->hh, &e->mm, &e->ss, &sg, &tzh, &tzm, &k) < 9) return 0;
    if (sg != '+' && sg != '-') return 0;
    sign = sg == '-' ? -1 : 1;
    e->mon = 0;
    for (int i = 0; i < 12; i++) if (!strcmp(mon, MON[i])) e->mon = i + 1;
    if (!e->mon || e->day < 1 || e->day > 31 || e->hh > 23 || e->mm > 59 || e->ss > 60) return 0;
    e->tz_min = sign * (tzh * 60 + tzm);
    p += k;
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    const char *q = strchr(p, '"');
    if (!q) return 0;
    char req[160];
    size_t rl = (size_t)(q - p);
    if (rl >= sizeof req) return 0;
    memcpy(req, p, rl);
    req[rl] = 0;
    if (sscanf(req, "%7s %63s %11s", e->method, e->path, e->proto) != 3) return 0;
    p = q + 1;
    char bytes[16];
    if (sscanf(p, " %d %15s", &e->status, bytes) != 2) return 0;
    if (e->status < 100 || e->status > 599) return 0;
    if (!strcmp(bytes, "-")) e->bytes = -1;
    else {
        char *end;
        e->bytes = strtol(bytes, &end, 10);
        if (*end) return 0;
    }
    e->epoch = days_from_civil(e->year, e->mon, e->day) * 86400L + e->hh * 3600L + e->mm * 60L + e->ss - e->tz_min * 60L;
    return 1;
}

typedef struct { char key[64]; long hits; long bytes; } Bucket;
static void bump(Bucket *b, int *n, const char *key, long bytes) {
    for (int i = 0; i < *n; i++) if (!strcmp(b[i].key, key)) { b[i].hits++; b[i].bytes += bytes; return; }
    snprintf(b[*n].key, sizeof b[*n].key, "%s", key);
    b[*n].hits = 1; b[*n].bytes = bytes;
    (*n)++;
}
static int by_hits(const void *a, const void *b) {
    const Bucket *x = a, *y = b;
    if (x->hits != y->hits) return x->hits < y->hits ? 1 : -1;
    return strcmp(x->key, y->key);
}

int main(void) {
    static const char *hosts[] = {"10.0.0.1", "10.0.0.7", "192.168.1.20", "203.0.113.9", "example.org"};
    static const char *paths[] = {"/", "/index.html", "/api/users", "/api/users/42", "/static/app.js", "/missing", "/login"};
    static const char *methods[] = {"GET", "GET", "GET", "POST", "HEAD"};
    static const int statuses[] = {200, 200, 200, 200, 301, 304, 404, 500, 403};
    Buf log = {0};
    int total_lines = 0;
    for (int i = 0; i < 200; i++) {
        char line[300];
        uint32_t a = rnd(), b = rnd(), c = rnd();
        int day = 27 + (int)(i / 40);     /* Feb 27, 28, Mar 1, 2, 3 across a month boundary */
        const char *mon = day > 28 ? "Mar" : "Feb";
        if (day > 28) day -= 28;
        int hh = (int)(a % 24), mm = (int)(b % 60), ss = (int)(c % 60);
        const char *tz = (a >> 8) % 3 == 0 ? "+0200" : (a >> 8) % 3 == 1 ? "-0500" : "+0000";
        int st = statuses[b % 9];
        char bytes[16];
        if (st == 304 || st == 301) snprintf(bytes, sizeof bytes, "-"); else snprintf(bytes, sizeof bytes, "%u", (unsigned)(200 + c % 5000));
        snprintf(line, sizeof line, "%s - %s [%02d/%s/2023:%02d:%02d:%02d %s] \"%s %s HTTP/1.1\" %d %s\n", hosts[a % 5],
                 (b >> 4) % 4 == 0 ? "alice" : "-", day, mon, hh, mm, ss, tz, methods[(c >> 3) % 5], paths[(a >> 12) % 7], st, bytes);
        bstr(&log, line);
        total_lines++;
    }
    /* malformed lines interleaved */
    static const char *junk[] = {
        "garbage line without structure\n",
        "1.2.3.4 - - [31/Foo/2023:10:00:00 +0000] \"GET / HTTP/1.1\" 200 12\n",
        "1.2.3.4 - - [31/Jan/2023:10:00:00 +0000] \"GET /\" 200 12\n",
        "1.2.3.4 - - [31/Jan/2023:10:00:00 +0000] \"GET / HTTP/1.1\" 999 12\n",
        "1.2.3.4 - - [31/Jan/2023:10:00:00 0000] \"GET / HTTP/1.1\" 200 12\n",
        "1.2.3.4 - - [31/Jan/2023:10:00:00 +0000] \"GET / HTTP/1.1\" 200 12x\n"
    };
    for (int i = 0; i < 6; i++) { bstr(&log, junk[i]); total_lines++; }
    wfile("access.log", log.p, log.n);
    size_t n;
    unsigned char *raw = rfile("access.log", &n);
    char *text = malloc(n + 1);
    memcpy(text, raw, n);
    text[n] = 0;

    Bucket st[16], pa[16], ho[16];
    int nst = 0, npa = 0, nho = 0;
    int ok = 0, bad = 0;
    long total_bytes = 0, unknown_size = 0;
    long min_epoch = 0x7FFFFFFFFFL, max_epoch = 0;
    int hourly[24] = {0};
    for (char *ln = text; *ln;) {
        char *e = strchr(ln, '\n');
        *e = 0;
        Entry en;
        if (parse_line(ln, &en)) {
            ok++;
            char key[64];
            snprintf(key, sizeof key, "%d", en.status);
            bump(st, &nst, key, en.bytes > 0 ? en.bytes : 0);
            bump(pa, &npa, en.path, en.bytes > 0 ? en.bytes : 0);
            bump(ho, &nho, en.host, en.bytes > 0 ? en.bytes : 0);
            if (en.bytes < 0) unknown_size++; else total_bytes += en.bytes;
            if (en.epoch < min_epoch) min_epoch = en.epoch;
            if (en.epoch > max_epoch) max_epoch = en.epoch;
            hourly[(en.epoch / 3600) % 24]++;
        } else bad++;
        ln = e + 1;
    }
    CHECK(ok == 200 && bad == 6 && ok + bad == total_lines);
    qsort(st, (size_t)nst, sizeof st[0], by_hits);
    qsort(pa, (size_t)npa, sizeof pa[0], by_hits);
    qsort(ho, (size_t)nho, sizeof ho[0], by_hits);
    printf("lines=%d parsed=%d rejected=%d\n", total_lines, ok, bad);
    printf("bytes served=%ld, entries without size=%ld\n", total_bytes, unknown_size);
    printf("time span: %ld s (first %ld, last %ld)\n", max_epoch - min_epoch, min_epoch, max_epoch);
    long sum = 0;
    for (int i = 0; i < nst; i++) { printf("status %s: %ld\n", st[i].key, st[i].hits); sum += st[i].hits; }
    CHECK(sum == ok);
    for (int i = 0; i < 3; i++) printf("top path %d: %-16s hits=%ld bytes=%ld\n", i + 1, pa[i].key, pa[i].hits, pa[i].bytes);
    for (int i = 0; i < 3; i++) printf("top host %d: %-14s hits=%ld\n", i + 1, ho[i].key, ho[i].hits);
    int busiest = 0;
    for (int h = 1; h < 24; h++) if (hourly[h] > hourly[busiest]) busiest = h;
    printf("busiest UTC hour: %02d:00 with %d requests\n", busiest, hourly[busiest]);
    /* epoch sanity: 2023-03-01T00:00:00Z */
    CHECK(days_from_civil(2023, 3, 1) * 86400L == 1677628800L);
    free(text); free(raw); bfree(&log);
    remove("access.log");
    return 0;
}
