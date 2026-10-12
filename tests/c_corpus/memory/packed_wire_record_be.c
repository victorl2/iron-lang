/*
 * title: Packed wire format with explicit big-endian byte layout
 * topic: memory
 * covers: manual serialization, network byte order, varint, checksum, round trip, truncation errors
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Wire layout (all multi-byte fields big-endian, no padding):
 *   0  u16 magic 0xC0DE
 *   2  u8  version
 *   3  u8  flags
 *   4  u32 seq
 *   8  i64 timestamp
 *  16  u16 name_len
 *  18  name bytes
 *  ..  u8  n_items, then n_items * (u16 key, i32 value)
 *  ..  u16 crc16 over everything before it
 */
typedef struct {
    uint8_t version, flags;
    uint32_t seq;
    int64_t ts;
    char name[32];
    uint8_t n_items;
    struct {
        uint16_t key;
        int32_t value;
    } items[8];
} Msg;

static uint16_t crc16(const unsigned char *p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

typedef struct {
    unsigned char *p;
    size_t cap, len;
    int err;
} W;

static void put(W *w, uint64_t v, int nbytes) {
    if (w->len + (size_t)nbytes > w->cap) {
        w->err = 1;
        return;
    }
    for (int i = nbytes - 1; i >= 0; i--)
        w->p[w->len++] = (unsigned char)(v >> (8 * i));
}

static size_t encode(const Msg *m, unsigned char *out, size_t cap) {
    W w = {out, cap, 0, 0};
    put(&w, 0xC0DE, 2);
    put(&w, m->version, 1);
    put(&w, m->flags, 1);
    put(&w, m->seq, 4);
    put(&w, (uint64_t)m->ts, 8);
    size_t nl = strlen(m->name);
    put(&w, nl, 2);
    for (size_t i = 0; i < nl; i++)
        put(&w, (unsigned char)m->name[i], 1);
    put(&w, m->n_items, 1);
    for (int i = 0; i < m->n_items; i++) {
        put(&w, m->items[i].key, 2);
        put(&w, (uint32_t)m->items[i].value, 4);
    }
    uint16_t c = crc16(out, w.len);
    put(&w, c, 2);
    return w.err ? 0 : w.len;
}

typedef struct {
    const unsigned char *p;
    size_t len, pos;
    int err;
} R;

static uint64_t get(R *r, int nbytes) {
    if (r->pos + (size_t)nbytes > r->len) {
        r->err = 1;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < nbytes; i++)
        v = (v << 8) | r->p[r->pos++];
    return v;
}

typedef enum { D_OK, D_SHORT, D_MAGIC, D_CRC, D_RANGE } Status;

static const char *status_name(Status s) {
    static const char *n[] = {"ok", "short", "bad-magic", "bad-crc", "out-of-range"};
    return n[s];
}

static Status decode(const unsigned char *buf, size_t len, Msg *m) {
    if (len < 4)
        return D_SHORT;
    uint16_t want = (uint16_t)((buf[len - 2] << 8) | buf[len - 1]);
    R r = {buf, len - 2, 0, 0};
    if (get(&r, 2) != 0xC0DE)
        return D_MAGIC;
    if (crc16(buf, len - 2) != want)
        return D_CRC;
    memset(m, 0, sizeof *m);
    m->version = (uint8_t)get(&r, 1);
    m->flags = (uint8_t)get(&r, 1);
    m->seq = (uint32_t)get(&r, 4);
    m->ts = (int64_t)get(&r, 8);
    size_t nl = (size_t)get(&r, 2);
    if (r.err)
        return D_SHORT;
    if (nl >= sizeof m->name)
        return D_RANGE;
    for (size_t i = 0; i < nl; i++)
        m->name[i] = (char)get(&r, 1);
    m->n_items = (uint8_t)get(&r, 1);
    if (r.err)
        return D_SHORT;
    if (m->n_items > 8)
        return D_RANGE;
    for (int i = 0; i < m->n_items; i++) {
        m->items[i].key = (uint16_t)get(&r, 2);
        m->items[i].value = (int32_t)(uint32_t)get(&r, 4);
    }
    return r.err ? D_SHORT : (r.pos == r.len ? D_OK : D_RANGE);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void hexdump(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        printf("%02x%s", p[i], (i % 16 == 15 || i + 1 == n) ? "\n" : " ");
}

int main(void) {
    Msg m;
    memset(&m, 0, sizeof m);
    m.version = 3;
    m.flags = 0x81;
    m.seq = 0x01020304u;
    m.ts = -1234567890123LL;
    strcpy(m.name, "sensor-A");
    m.n_items = 3;
    m.items[0].key = 1;
    m.items[0].value = -5;
    m.items[1].key = 0xABCD;
    m.items[1].value = 2147483647;
    m.items[2].key = 65535;
    m.items[2].value = -2147483647 - 1;

    unsigned char buf[128];
    size_t n = encode(&m, buf, sizeof buf);
    check(n > 0, "encode");
    printf("encoded %zu bytes\n", n);
    hexdump(buf, n);

    Msg d;
    Status s = decode(buf, n, &d);
    printf("decode: %s\n", status_name(s));
    check(s == D_OK, "decode ok");
    check(d.seq == m.seq && d.ts == m.ts && d.version == 3 && d.flags == 0x81, "scalars");
    check(strcmp(d.name, m.name) == 0, "name");
    check(d.n_items == 3, "n items");
    for (int i = 0; i < 3; i++)
        check(d.items[i].key == m.items[i].key && d.items[i].value == m.items[i].value, "items");
    printf("round trip: seq=%u ts=%lld items=%d\n", (unsigned)d.seq, (long long)d.ts, d.n_items);

    /* every truncation must be rejected and never read out of bounds */
    int rejected = 0;
    for (size_t cut = 0; cut < n; cut++) {
        Msg t;
        Status st = decode(buf, cut, &t);
        check(st != D_OK, "truncation rejected");
        rejected++;
    }
    printf("truncations rejected: %d\n", rejected);

    /* every single-bit flip is caught by magic or CRC */
    int caught = 0, flips = 0;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) {
            unsigned char c[128];
            memcpy(c, buf, n);
            c[i] ^= (unsigned char)(1u << b);
            Msg t;
            Status st = decode(c, n, &t);
            flips++;
            if (st != D_OK)
                caught++;
        }
    printf("bit flips caught: %d of %d\n", caught, flips);
    check(caught == flips, "all flips caught");

    /* encode into a too-small buffer fails cleanly */
    unsigned char tiny[10];
    check(encode(&m, tiny, sizeof tiny) == 0, "overflow refused");
    printf("small buffer: refused\n");
    return 0;
}
