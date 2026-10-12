/*
 * title: Padding bytes make memcmp and raw hashing of structs unreliable
 * topic: memory
 * covers: padding map from offsetof, raw-byte comparison hazard, field-wise equality, canonical serialization, union tail bytes
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t tag;
    uint32_t id;
    uint8_t flag;
    uint64_t stamp;
    uint16_t port;
} Rec;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct {
    size_t off, len;
} Range;

/* padding ranges derived from offsetof/sizeof of each member */
static int padding_ranges(Range *out) {
    struct {
        size_t off, size;
    } f[] = {
        {offsetof(Rec, tag), 1}, {offsetof(Rec, id), 4}, {offsetof(Rec, flag), 1},
        {offsetof(Rec, stamp), 8}, {offsetof(Rec, port), 2},
    };
    int n = 0;
    size_t end = 0;
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (f[i].off > end) {
            out[n].off = end;
            out[n].len = f[i].off - end;
            n++;
        }
        end = f[i].off + f[i].size;
    }
    if (sizeof(Rec) > end) {
        out[n].off = end;
        out[n].len = sizeof(Rec) - end;
        n++;
    }
    return n;
}

/* build a Rec image byte by byte: fields via memcpy at their offsets, padding filled with `fill` */
static void make_image(unsigned char *img, unsigned char fill, uint8_t tag, uint32_t id, uint8_t flag,
                       uint64_t stamp, uint16_t port) {
    memset(img, fill, sizeof(Rec));
    memcpy(img + offsetof(Rec, tag), &tag, sizeof tag);
    memcpy(img + offsetof(Rec, id), &id, sizeof id);
    memcpy(img + offsetof(Rec, flag), &flag, sizeof flag);
    memcpy(img + offsetof(Rec, stamp), &stamp, sizeof stamp);
    memcpy(img + offsetof(Rec, port), &port, sizeof port);
}

static int rec_equal(const Rec *a, const Rec *b) {
    return a->tag == b->tag && a->id == b->id && a->flag == b->flag && a->stamp == b->stamp && a->port == b->port;
}

static uint32_t fnv(const unsigned char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* canonical bytes: fields only, fixed little-endian order, no padding */
static size_t canonical(const Rec *r, unsigned char *out) {
    size_t n = 0;
    out[n++] = r->tag;
    for (int i = 0; i < 4; i++)
        out[n++] = (unsigned char)(r->id >> (8 * i));
    out[n++] = r->flag;
    for (int i = 0; i < 8; i++)
        out[n++] = (unsigned char)(r->stamp >> (8 * i));
    for (int i = 0; i < 2; i++)
        out[n++] = (unsigned char)(r->port >> (8 * i));
    return n;
}

int main(void) {
    Range pad[8];
    int np = padding_ranges(pad);
    size_t padbytes = 0;
    printf("sizeof(Rec)=%zu, padding ranges:\n", sizeof(Rec));
    for (int i = 0; i < np; i++) {
        printf("  [%zu, %zu) %zu bytes\n", pad[i].off, pad[i].off + pad[i].len, pad[i].len);
        padbytes += pad[i].len;
    }
    printf("padding total %zu of %zu bytes (%zu payload)\n", padbytes, sizeof(Rec), sizeof(Rec) - padbytes);
    check(sizeof(Rec) - padbytes == 1 + 4 + 1 + 8 + 2, "payload accounting");

    /* two images with identical field values but different padding contents */
    unsigned char ia[sizeof(Rec)], ib[sizeof(Rec)];
    make_image(ia, 0x00, 7, 0xCAFEBABEu, 1, 0x1122334455667788ull, 8080);
    make_image(ib, 0xFF, 7, 0xCAFEBABEu, 1, 0x1122334455667788ull, 8080);
    Rec a, b;
    memcpy(&a, ia, sizeof a);
    memcpy(&b, ib, sizeof b);
    printf("fieldwise equal: %s\n", rec_equal(&a, &b) ? "yes" : "no");
    printf("raw memcmp equal: %s\n", memcmp(ia, ib, sizeof ia) == 0 ? "yes" : "no");
    check(rec_equal(&a, &b), "fields equal");
    check(memcmp(ia, ib, sizeof ia) != 0, "raw bytes differ in padding");

    /* raw hash differs, canonical hash agrees */
    uint32_t raw_a = fnv(ia, sizeof ia), raw_b = fnv(ib, sizeof ib);
    unsigned char ca[64], cb[64];
    size_t la = canonical(&a, ca), lb = canonical(&b, cb);
    uint32_t can_a = fnv(ca, la), can_b = fnv(cb, lb);
    printf("raw hash differs: %s\n", raw_a != raw_b ? "yes" : "no");
    printf("canonical length %zu, hashes equal: %s (0x%08x)\n", la, can_a == can_b ? "yes" : "no", (unsigned)can_a);
    check(raw_a != raw_b && can_a == can_b && la == lb, "hash behaviour");

    /* how many padding-only byte flips does raw comparison see? */
    unsigned flips_seen = 0, flips_total = 0;
    for (int r = 0; r < np; r++)
        for (size_t k = 0; k < pad[r].len; k++) {
            unsigned char t[sizeof(Rec)];
            memcpy(t, ia, sizeof t);
            t[pad[r].off + k] ^= 0x01;
            flips_total++;
            Rec x;
            memcpy(&x, t, sizeof x);
            if (memcmp(t, ia, sizeof t) != 0 && rec_equal(&x, &a))
                flips_seen++;
        }
    printf("padding bit flips: %u, each invisible to fields but visible to memcmp: %u\n", flips_total, flips_seen);
    check(flips_total == flips_seen && flips_total == padbytes, "every padding byte is observable only raw");

    /* payload flips are seen by both */
    unsigned both = 0, payload_total = 0;
    for (size_t off = 0; off < sizeof(Rec); off++) {
        int is_pad = 0;
        for (int r = 0; r < np; r++)
            if (off >= pad[r].off && off < pad[r].off + pad[r].len)
                is_pad = 1;
        if (is_pad)
            continue;
        unsigned char t[sizeof(Rec)];
        memcpy(t, ia, sizeof t);
        t[off] ^= 0x80;
        Rec x;
        memcpy(&x, t, sizeof x);
        payload_total++;
        if (!rec_equal(&x, &a) && memcmp(t, ia, sizeof t) != 0)
            both++;
    }
    printf("payload byte flips detected by both methods: %u of %u\n", both, payload_total);
    check(both == payload_total, "payload flips");

    /* a sort by raw bytes disagrees with a sort by fields when only padding differs */
    unsigned char imgs[4][sizeof(Rec)];
    make_image(imgs[0], 0x11, 1, 100, 0, 5, 1);
    make_image(imgs[1], 0x00, 1, 100, 0, 5, 1);
    make_image(imgs[2], 0xEE, 1, 100, 0, 5, 1);
    make_image(imgs[3], 0x77, 1, 100, 0, 6, 1);
    int eq_fields = 0, eq_raw = 0;
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) {
            Rec x, y;
            memcpy(&x, imgs[i], sizeof x);
            memcpy(&y, imgs[j], sizeof y);
            eq_fields += rec_equal(&x, &y);
            eq_raw += memcmp(imgs[i], imgs[j], sizeof x) == 0;
        }
    printf("equal pairs of 6: by fields %d, by raw bytes %d\n", eq_fields, eq_raw);
    check(eq_fields == 3 && eq_raw == 0, "pair counts");

    /* union tail bytes: writing the small member leaves the rest of the union untouched */
    union {
        uint8_t small;
        uint64_t big;
    } u;
    unsigned char raw[sizeof u];
    memset(raw, 0xAB, sizeof raw);
    memcpy(&u, raw, sizeof u);
    uint8_t v = 0x11;
    memcpy(&u, &v, 1); /* store through the byte only */
    unsigned char after[sizeof u];
    memcpy(after, &u, sizeof u);
    unsigned untouched = 0;
    for (size_t i = 1; i < sizeof after; i++)
        untouched += after[i] == 0xAB;
    printf("union: first byte 0x%02x, untouched tail bytes %u of %zu\n", (unsigned)after[0], untouched,
           sizeof after - 1);
    check(untouched == sizeof after - 1, "tail untouched");
    return 0;
}
