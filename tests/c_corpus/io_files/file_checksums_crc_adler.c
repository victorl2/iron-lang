/*
 * title: Streaming file checksums CRC32 Adler32 Fletcher
 * topic: io_files
 * covers: table-driven CRC-32, Adler-32, Fletcher-16, known answer vectors, chunk-size independence
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLETCHER_KAT 0x1EDEu
static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

typedef struct {
    uint32_t crc;
    uint32_t a, b;
    uint32_t f1, f2;
    unsigned long long bytes;
} Sums;

static void sums_init(Sums *s) {
    s->crc = 0xFFFFFFFFu;
    s->a = 1;
    s->b = 0;
    s->f1 = s->f2 = 0;
    s->bytes = 0;
}

static void sums_update(Sums *s, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        s->crc = crc_table[(s->crc ^ p[i]) & 0xFFu] ^ (s->crc >> 8);
        s->a = (s->a + p[i]) % 65521u;
        s->b = (s->b + s->a) % 65521u;
        s->f1 = (s->f1 + p[i]) % 255u;
        s->f2 = (s->f2 + s->f1) % 255u;
    }
    s->bytes += n;
}

static uint32_t sums_crc(const Sums *s) { return s->crc ^ 0xFFFFFFFFu; }
static uint32_t sums_adler(const Sums *s) { return (s->b << 16) | s->a; }
static uint32_t sums_fletcher(const Sums *s) { return (s->f2 << 8) | s->f1; }

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static Sums file_sums(const char *name, size_t chunk) {
    Sums s;
    sums_init(&s);
    FILE *f = fopen(name, "rb");
    check(f != NULL, "open");
    unsigned char *buf = malloc(chunk);
    check(buf != NULL, "alloc");
    size_t n;
    while ((n = fread(buf, 1, chunk, f)) > 0)
        sums_update(&s, buf, n);
    free(buf);
    fclose(f);
    return s;
}

static void write_file(const char *name, const void *p, size_t n) {
    FILE *f = fopen(name, "wb");
    check(f != NULL, "write open");
    if (n > 0)
        check(fwrite(p, 1, n, f) == n, "write");
    fclose(f);
}

int main(void) {
    crc_init();

    /* published check values for the ASCII string "123456789" */
    write_file("kat.bin", "123456789", 9);
    Sums s = file_sums("kat.bin", 4);
    printf("\"123456789\": crc32 %08x adler32 %08x fletcher16 %04x\n", (unsigned)sums_crc(&s),
           (unsigned)sums_adler(&s), (unsigned)sums_fletcher(&s));
    check(sums_crc(&s) == 0xCBF43926u, "crc32 kat");
    check(sums_adler(&s) == 0x091E01DEu, "adler32 kat");
    check(sums_fletcher(&s) == FLETCHER_KAT, "fletcher16 kat");

    /* more known answers */
    write_file("kat.bin", "The quick brown fox jumps over the lazy dog", 43);
    s = file_sums("kat.bin", 5);
    printf("fox: crc32 %08x adler32 %08x\n", (unsigned)sums_crc(&s), (unsigned)sums_adler(&s));
    check(sums_crc(&s) == 0x414FA339u, "fox crc");
    check(sums_adler(&s) == 0x5BDC0FDAu, "fox adler");

    write_file("kat.bin", "", 0);
    s = file_sums("kat.bin", 16);
    printf("empty: crc32 %08x adler32 %08x\n", (unsigned)sums_crc(&s), (unsigned)sums_adler(&s));
    check(sums_crc(&s) == 0 && sums_adler(&s) == 1, "empty");

    write_file("kat.bin", "abcde", 5);
    s = file_sums("kat.bin", 2);
    printf("\"abcde\": fletcher16 %04x\n", (unsigned)sums_fletcher(&s));
    check(sums_fletcher(&s) == 0xC8F0u, "fletcher abcde");

    write_file("kat.bin", "a", 1);
    s = file_sums("kat.bin", 16);
    check(sums_crc(&s) == 0xE8B7BE43u && sums_adler(&s) == 0x00620062u, "a");
    printf("\"a\": crc32 %08x adler32 %08x\n", (unsigned)sums_crc(&s), (unsigned)sums_adler(&s));

    /* larger pseudo-random file: chunk size must not matter */
    unsigned long long st = 0x123456789ABCDEFULL;
    size_t n = 100003;
    unsigned char *data = malloc(n);
    check(data != NULL, "alloc data");
    for (size_t i = 0; i < n; i++) {
        st = st * 6364136223846793005ULL + 1442695040888963407ULL;
        data[i] = (unsigned char)(st >> 56);
    }
    write_file("big.bin", data, n);
    Sums ref = file_sums("big.bin", 1 << 16);
    printf("big file %llu bytes: crc32 %08x adler32 %08x fletcher16 %04x\n", ref.bytes,
           (unsigned)sums_crc(&ref), (unsigned)sums_adler(&ref), (unsigned)sums_fletcher(&ref));
    size_t chunks[] = {1, 3, 255, 4096, 65521};
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        Sums t = file_sums("big.bin", chunks[i]);
        int same = sums_crc(&t) == sums_crc(&ref) && sums_adler(&t) == sums_adler(&ref) &&
                   sums_fletcher(&t) == sums_fletcher(&ref) && t.bytes == ref.bytes;
        printf("chunk %5zu: %s\n", chunks[i], same ? "same" : "DIFFERENT");
        check(same, "chunk independence");
    }

    /* flipping one bit changes every checksum */
    data[50000] ^= 0x10;
    write_file("big.bin", data, n);
    Sums flipped = file_sums("big.bin", 4096);
    printf("after one bit flip: crc32 %08x adler32 %08x\n", (unsigned)sums_crc(&flipped),
           (unsigned)sums_adler(&flipped));
    check(sums_crc(&flipped) != sums_crc(&ref) && sums_adler(&flipped) != sums_adler(&ref), "flip");
    free(data);
    remove("kat.bin");
    remove("big.bin");
    return 0;
}
