/*
 * title: WAV RIFF writer and reader for 8/16/24-bit PCM
 * topic: io_files
 * covers: riff chunks, fmt header, pcm 8/16/24-bit, stereo interleave, odd chunk padding, unknown chunks, peak/rms in integers
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
    int channels, rate, bits;
    size_t frames;
    int32_t *s;     /* interleaved, signed, normalised to the native bit depth range */
} Wav;

static void le16(Buf *b, unsigned v) { bbyte(b, v & 255u); bbyte(b, (v >> 8) & 255u); }
static void le32(Buf *b, uint32_t v) { le16(b, v & 0xFFFFu); le16(b, v >> 16); }
static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

/* Triangle wave, integer-only so output is identical everywhere. */
static int32_t tri(size_t i, size_t period, int32_t amp) {
    size_t ph = i % period, half = period / 2;
    int64_t v = ph < half ? (int64_t)ph * 2 * amp / (int64_t)half - amp
                          : amp - (int64_t)(ph - half) * 2 * amp / (int64_t)half;
    return (int32_t)v;
}

static void write_wav(const char *name, const Wav *w, int add_list_chunk) {
    Buf d = {0};
    int bytes = w->bits / 8;
    for (size_t i = 0; i < w->frames * (size_t)w->channels; i++) {
        int32_t v = w->s[i];
        if (bytes == 1) bbyte(&d, (unsigned)(v + 128));           /* 8-bit is unsigned */
        else for (int k = 0; k < bytes; k++) bbyte(&d, ((uint32_t)v >> (8 * k)) & 255u);
    }
    Buf b = {0};
    bstr(&b, "RIFF"); le32(&b, 0); bstr(&b, "WAVE");
    bstr(&b, "fmt "); le32(&b, 16);
    le16(&b, 1); le16(&b, (unsigned)w->channels); le32(&b, (uint32_t)w->rate);
    le32(&b, (uint32_t)(w->rate * w->channels * bytes));
    le16(&b, (unsigned)(w->channels * bytes)); le16(&b, (unsigned)w->bits);
    if (add_list_chunk) {
        bstr(&b, "LIST"); le32(&b, 5); bstr(&b, "abcde"); bbyte(&b, 0);   /* odd size + pad byte */
    }
    bstr(&b, "data"); le32(&b, (uint32_t)d.n);
    bput(&b, d.p, d.n);
    if (d.n & 1) bbyte(&b, 0);
    uint32_t riff = (uint32_t)b.n - 8;
    for (int k = 0; k < 4; k++) b.p[4 + k] = (unsigned char)(riff >> (8 * k));
    wfile(name, b.p, b.n);
    bfree(&b); bfree(&d);
}

static const char *read_wav(const char *name, Wav *w, int *chunks_seen) {
    size_t n;
    unsigned char *d = rfile(name, &n);
    const char *err = NULL;
    int have_fmt = 0, have_data = 0;
    *chunks_seen = 0;
    w->s = NULL;
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) err = "not RIFF/WAVE";
    else if (rd32(d + 4) + 8 != n) err = "RIFF size mismatch";
    size_t pos = 12;
    while (!err && pos + 8 <= n) {
        uint32_t sz = rd32(d + pos + 4);
        const unsigned char *body = d + pos + 8;
        if (pos + 8 + sz > n) { err = "chunk overruns file"; break; }
        (*chunks_seen)++;
        if (!memcmp(d + pos, "fmt ", 4)) {
            if (sz < 16 || rd16(body) != 1) { err = "not PCM"; break; }
            w->channels = (int)rd16(body + 2);
            w->rate = (int)rd32(body + 4);
            w->bits = (int)rd16(body + 14);
            if (rd16(body + 12) != (uint32_t)(w->channels * w->bits / 8)) { err = "bad block align"; break; }
            if (w->bits != 8 && w->bits != 16 && w->bits != 24) { err = "unsupported bit depth"; break; }
            have_fmt = 1;
        } else if (!memcmp(d + pos, "data", 4)) {
            if (!have_fmt) { err = "data before fmt"; break; }
            int bytes = w->bits / 8;
            if (sz % (uint32_t)(bytes * w->channels)) { err = "partial frame"; break; }
            w->frames = sz / (size_t)(bytes * w->channels);
            w->s = malloc(sz / (size_t)bytes * sizeof(int32_t));
            for (size_t i = 0; i < sz / (size_t)bytes; i++) {
                uint32_t v = 0;
                for (int k = 0; k < bytes; k++) v |= (uint32_t)body[i * (size_t)bytes + (size_t)k] << (8 * k);
                if (bytes == 1) w->s[i] = (int32_t)v - 128;
                else if (bytes == 2) w->s[i] = (int32_t)(v ^ 0x8000u) - 0x8000;
                else w->s[i] = (int32_t)(v ^ 0x800000u) - 0x800000;
            }
            have_data = 1;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!err && !have_data) err = "no data chunk";
    if (err && w->s) { free(w->s); w->s = NULL; }
    free(d);
    return err;
}

int main(void) {
    static const struct { int ch, rate, bits; size_t frames; int list; } cfg[] = {
        {1, 8000, 8, 37, 1}, {2, 44100, 16, 50, 0}, {2, 48000, 24, 33, 1}, {1, 22050, 16, 101, 1}
    };
    for (int t = 0; t < 4; t++) {
        Wav w = {cfg[t].ch, cfg[t].rate, cfg[t].bits, cfg[t].frames, NULL};
        int32_t amp = (1 << (w.bits - 1)) - 1;
        if (w.bits == 8) amp = 100;
        w.s = malloc(w.frames * (size_t)w.channels * sizeof(int32_t));
        for (size_t i = 0; i < w.frames; i++)
            for (int c = 0; c < w.channels; c++)
                w.s[i * (size_t)w.channels + (size_t)c] = tri(i + (size_t)c * 5, 16 + (size_t)c * 8, amp);
        write_wav("t.wav", &w, cfg[t].list);
        Wav r;
        int seen;
        const char *e = read_wav("t.wav", &r, &seen);
        CHECK(e == NULL);
        CHECK(r.channels == w.channels && r.rate == w.rate && r.bits == w.bits && r.frames == w.frames);
        CHECK(memcmp(r.s, w.s, w.frames * (size_t)w.channels * sizeof(int32_t)) == 0);
        int32_t peak = 0;
        int64_t sq = 0;
        size_t total = r.frames * (size_t)r.channels;
        for (size_t i = 0; i < total; i++) {
            int32_t a = r.s[i] < 0 ? -r.s[i] : r.s[i];
            if (a > peak) peak = a;
            sq += (int64_t)r.s[i] * r.s[i];
        }
        size_t fsz;
        free(rfile("t.wav", &fsz));
        printf("ch=%d rate=%5d bits=%2d frames=%3zu chunks=%d file=%zu peak=%d mean-square=%lld\n",
               r.channels, r.rate, r.bits, r.frames, seen, fsz, peak, (long long)(sq / (int64_t)total));
        free(w.s); free(r.s);
    }
    /* header corruption */
    Wav w = {2, 8000, 16, 4, malloc(8 * sizeof(int32_t))};
    for (int i = 0; i < 8; i++) w.s[i] = i * 1000 - 3500;
    write_wav("g.wav", &w, 0);
    size_t n;
    unsigned char *raw = rfile("g.wav", &n);
    struct { size_t off; unsigned char v; } bad[] = {{0, 'X'}, {4, 0}, {20, 3}, {32, 7}, {34, 12}, {40, 5}};
    for (int i = 0; i < 6; i++) {
        unsigned char *c = malloc(n);
        memcpy(c, raw, n);
        c[bad[i].off] = bad[i].v;
        wfile("g2.wav", c, n);
        Wav r;
        int seen;
        const char *e = read_wav("g2.wav", &r, &seen);
        printf("patch %2zu: %s\n", bad[i].off, e ? e : "accepted");
        if (!e) free(r.s);
        free(c);
    }
    free(raw); free(w.s);
    remove("t.wav"); remove("g.wav"); remove("g2.wav");
    return 0;
}
