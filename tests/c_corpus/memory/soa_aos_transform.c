/*
 * title: Array-of-structs to struct-of-arrays transform
 * topic: memory
 * covers: AoS vs SoA, layout conversion, strided access, field-wise kernels, round trip, byte footprints
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t id;
    float mass;
    float pos[3];
    uint8_t alive;
    uint16_t group;
} Particle;

typedef struct {
    size_t n;
    int32_t *id;
    float *mass;
    float *px, *py, *pz;
    uint8_t *alive;
    uint16_t *group;
    void *block; /* single allocation holding every column */
} ParticlesSoA;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static size_t align8(size_t x) {
    return (x + 7) & ~(size_t)7;
}

/* One allocation, columns placed back to back with 8-byte alignment. */
static int soa_alloc(ParticlesSoA *s, size_t n) {
    size_t o_id = 0;
    size_t o_mass = align8(o_id + n * sizeof(int32_t));
    size_t o_px = align8(o_mass + n * sizeof(float));
    size_t o_py = align8(o_px + n * sizeof(float));
    size_t o_pz = align8(o_py + n * sizeof(float));
    size_t o_alive = align8(o_pz + n * sizeof(float));
    size_t o_group = align8(o_alive + n * sizeof(uint8_t));
    size_t total = align8(o_group + n * sizeof(uint16_t));
    unsigned char *b = calloc(1, total ? total : 1);
    if (!b)
        return -1;
    s->n = n;
    s->block = b;
    s->id = (int32_t *)(void *)(b + o_id);
    s->mass = (float *)(void *)(b + o_mass);
    s->px = (float *)(void *)(b + o_px);
    s->py = (float *)(void *)(b + o_py);
    s->pz = (float *)(void *)(b + o_pz);
    s->alive = b + o_alive;
    s->group = (uint16_t *)(void *)(b + o_group);
    return 0;
}

static void aos_to_soa(const Particle *a, ParticlesSoA *s) {
    for (size_t i = 0; i < s->n; i++) {
        s->id[i] = a[i].id;
        s->mass[i] = a[i].mass;
        s->px[i] = a[i].pos[0];
        s->py[i] = a[i].pos[1];
        s->pz[i] = a[i].pos[2];
        s->alive[i] = a[i].alive;
        s->group[i] = a[i].group;
    }
}

static void soa_to_aos(const ParticlesSoA *s, Particle *a) {
    memset(a, 0, s->n * sizeof *a);
    for (size_t i = 0; i < s->n; i++) {
        a[i].id = s->id[i];
        a[i].mass = s->mass[i];
        a[i].pos[0] = s->px[i];
        a[i].pos[1] = s->py[i];
        a[i].pos[2] = s->pz[i];
        a[i].alive = s->alive[i];
        a[i].group = s->group[i];
    }
}

/* the same kernels written against both layouts; results must agree exactly */
static double total_mass_aos(const Particle *a, size_t n) {
    double t = 0;
    for (size_t i = 0; i < n; i++)
        if (a[i].alive)
            t += a[i].mass;
    return t;
}

static double total_mass_soa(const ParticlesSoA *s) {
    double t = 0;
    for (size_t i = 0; i < s->n; i++)
        if (s->alive[i])
            t += s->mass[i];
    return t;
}

static void centroid_aos(const Particle *a, size_t n, double c[3]) {
    double m = 0;
    c[0] = c[1] = c[2] = 0;
    for (size_t i = 0; i < n; i++) {
        if (!a[i].alive)
            continue;
        m += a[i].mass;
        for (int k = 0; k < 3; k++)
            c[k] += (double)a[i].mass * a[i].pos[k];
    }
    for (int k = 0; k < 3; k++)
        c[k] /= m;
}

static void centroid_soa(const ParticlesSoA *s, double c[3]) {
    double m = 0;
    c[0] = c[1] = c[2] = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (!s->alive[i])
            continue;
        m += s->mass[i];
        c[0] += (double)s->mass[i] * s->px[i];
        c[1] += (double)s->mass[i] * s->py[i];
        c[2] += (double)s->mass[i] * s->pz[i];
    }
    for (int k = 0; k < 3; k++)
        c[k] /= m;
}

static uint32_t rs = 4242;

static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

int main(void) {
    enum { N = 1000, G = 8 };
    Particle *a = calloc(N, sizeof *a);
    check(a != NULL, "calloc");
    for (int i = 0; i < N; i++) {
        a[i].id = i * 2 + 1;
        a[i].mass = (float)(1 + rnd() % 16) / 4.0f;      /* exact in binary */
        for (int k = 0; k < 3; k++)
            a[i].pos[k] = (float)((int)(rnd() % 129) - 64) / 8.0f; /* exact in binary */
        a[i].alive = (uint8_t)((rnd() % 5) != 0);
        a[i].group = (uint16_t)(rnd() % G);
    }
    printf("AoS: %zu bytes/element, %zu bytes total\n", sizeof(Particle), sizeof(Particle) * N);
    size_t payload = sizeof(int32_t) + sizeof(float) * 4 + 1 + 2;
    printf("payload per element: %zu, padding waste per element: %zu\n", payload, sizeof(Particle) - payload);

    ParticlesSoA s;
    check(soa_alloc(&s, N) == 0, "soa alloc");
    aos_to_soa(a, &s);
    size_t soa_bytes = N * payload;
    printf("SoA: %zu payload bytes across 7 columns\n", soa_bytes);

    printf("total mass  AoS=%.4f SoA=%.4f\n", total_mass_aos(a, N), total_mass_soa(&s));
    check(total_mass_aos(a, N) == total_mass_soa(&s), "mass agrees");
    double ca[3], cs[3];
    centroid_aos(a, N, ca);
    centroid_soa(&s, cs);
    printf("centroid    AoS=(%.4f, %.4f, %.4f)\n", ca[0], ca[1], ca[2]);
    printf("            SoA=(%.4f, %.4f, %.4f)\n", cs[0], cs[1], cs[2]);
    for (int k = 0; k < 3; k++)
        check(ca[k] == cs[k], "centroid agrees");

    /* group histogram using only two columns of the SoA layout */
    unsigned long hist[G] = {0};
    for (size_t i = 0; i < s.n; i++)
        if (s.alive[i])
            hist[s.group[i]]++;
    printf("alive per group:");
    unsigned long alive = 0;
    for (int g = 0; g < G; g++) {
        printf(" %lu", hist[g]);
        alive += hist[g];
    }
    printf("\n");
    unsigned long alive_aos = 0;
    for (int i = 0; i < N; i++)
        alive_aos += a[i].alive;
    check(alive == alive_aos, "alive count");

    /* mutate in SoA form, convert back, check against direct edits on AoS */
    for (size_t i = 0; i < s.n; i++) {
        s.px[i] += 1.5f;
        a[i].pos[0] += 1.5f;
        if (s.group[i] == 3) {
            s.alive[i] = 0;
            a[i].alive = 0;
        }
    }
    Particle *back = calloc(N, sizeof *back);
    check(back != NULL, "calloc back");
    soa_to_aos(&s, back);
    for (int i = 0; i < N; i++) {
        check(back[i].id == a[i].id && back[i].mass == a[i].mass, "id mass");
        check(back[i].pos[0] == a[i].pos[0] && back[i].pos[1] == a[i].pos[1] && back[i].pos[2] == a[i].pos[2], "pos");
        check(back[i].alive == a[i].alive && back[i].group == a[i].group, "flags");
    }
    printf("round trip after mutation: identical\n");
    printf("mass after mutation: %.4f\n", total_mass_soa(&s));

    free(back);
    free(s.block);
    free(a);
    return 0;
}
