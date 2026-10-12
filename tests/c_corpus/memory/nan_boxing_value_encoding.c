/*
 * title: NaN-boxed dynamic values in 64 bits
 * topic: memory
 * covers: NaN boxing, tagged payloads in the double bit space, NaN canonicalization, packed short strings, handle tables
 * deps: libc, libm
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A Value is a uint64_t. Anything whose top 13 bits are all ones (a negative quiet NaN)
 * is a boxed non-number: bits 48..50 hold the tag and bits 0..47 the payload.
 * Everything else is an ordinary double. Real NaNs are canonicalized to 0x7FF8000000000000
 * because hardware may produce 0xFFF8000000000000, which would collide with boxed int 0.
 */
typedef uint64_t Value;

#define BOX_MASK 0xFFF8000000000000ull
enum { T_INT = 0, T_BOOL = 1, T_NULL = 2, T_HANDLE = 3, T_SSTR = 4 };

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int is_boxed(Value v) {
    return (v & BOX_MASK) == BOX_MASK;
}

static unsigned tag_of(Value v) {
    return (unsigned)((v >> 48) & 7u);
}

static Value box(unsigned tag, uint64_t payload) {
    return BOX_MASK | ((uint64_t)tag << 48) | (payload & 0xFFFFFFFFFFFFull);
}

static Value from_double(double d) {
    uint64_t u;
    if (d != d)
        return 0x7FF8000000000000ull;
    memcpy(&u, &d, sizeof u);
    return u;
}

static double to_double(Value v) {
    double d;
    memcpy(&d, &v, sizeof d);
    return d;
}

static Value from_int(int32_t i) {
    return box(T_INT, (uint32_t)i);
}

static int32_t to_int(Value v) {
    return (int32_t)(uint32_t)(v & 0xFFFFFFFFu);
}

static Value from_bool(int b) {
    return box(T_BOOL, b != 0);
}

static Value make_null(void) {
    return box(T_NULL, 0);
}

static Value from_handle(uint32_t h) {
    return box(T_HANDLE, h);
}

/* up to 6 characters packed little-endian into the 48-bit payload */
static int from_sstr(const char *s, Value *out) {
    size_t n = strlen(s);
    if (n > 6)
        return 0;
    uint64_t p = 0;
    for (size_t i = 0; i < n; i++)
        p |= (uint64_t)(unsigned char)s[i] << (8 * i);
    *out = box(T_SSTR, p);
    return 1;
}

static void sstr_get(Value v, char out[7]) {
    for (int i = 0; i < 6; i++)
        out[i] = (char)((v >> (8 * i)) & 0xFF);
    out[6] = 0;
}

static const char *kind(Value v) {
    if (!is_boxed(v))
        return "number";
    static const char *n[] = {"int", "bool", "null", "handle", "sstr", "?", "?", "?"};
    return n[tag_of(v)];
}

/* addition with JS-like coercion: ints and doubles add as numbers, others are errors */
static int add(Value a, Value b, Value *out) {
    double x, y;
    if (is_boxed(a)) {
        if (tag_of(a) != T_INT)
            return 0;
        x = (double)to_int(a);
    } else {
        x = to_double(a);
    }
    if (is_boxed(b)) {
        if (tag_of(b) != T_INT)
            return 0;
        y = (double)to_int(b);
    } else {
        y = to_double(b);
    }
    *out = from_double(x + y);
    return 1;
}

static void show(Value v) {
    if (!is_boxed(v)) {
        double d = to_double(v);
        if (d != d)
            printf("nan");
        else if (isinf(d))
            printf(d < 0 ? "-inf" : "inf");
        else
            printf("%.4f", d);
        return;
    }
    char s[7];
    switch (tag_of(v)) {
    case T_INT: printf("%d", (int)to_int(v)); break;
    case T_BOOL: printf("%s", (v & 1) ? "true" : "false"); break;
    case T_NULL: printf("null"); break;
    case T_HANDLE: printf("#%u", (unsigned)(v & 0xFFFFFFFFu)); break;
    case T_SSTR: sstr_get(v, s); printf("'%s'", s); break;
    default: printf("?"); break;
    }
}

int main(void) {
    volatile double zero = 0.0;
    double nan_from_hw = zero / zero;   /* sign of the NaN differs across CPUs */
    double inf_val = 1.0 / zero;
    Value vals[16];
    int n = 0;
    vals[n++] = from_double(3.25);
    vals[n++] = from_double(-0.5);
    vals[n++] = from_double(1e15);
    vals[n++] = from_double(inf_val);
    vals[n++] = from_double(-inf_val);
    vals[n++] = from_double(nan_from_hw);
    vals[n++] = from_int(0);
    vals[n++] = from_int(-1);
    vals[n++] = from_int(2147483647);
    vals[n++] = from_int(-2147483647 - 1);
    vals[n++] = from_bool(1);
    vals[n++] = from_bool(0);
    vals[n++] = make_null();
    vals[n++] = from_handle(4000000000u);
    Value sv;
    check(from_sstr("hello", &sv), "sstr");
    vals[n++] = sv;
    check(from_sstr("sixsix", &sv), "sstr6");
    vals[n++] = sv;
    check(n == 16, "count");

    for (int i = 0; i < n; i++) {
        printf("%2d %-6s ", i, kind(vals[i]));
        show(vals[i]);
        printf("\n");
    }

    /* a NaN from hardware must never look boxed */
    check(!is_boxed(from_double(nan_from_hw)), "nan canonicalized");
    check(!is_boxed(from_double(inf_val)) && !is_boxed(from_double(-inf_val)), "infinities are numbers");
    check(from_int(0) != from_double(nan_from_hw), "int 0 differs from hardware NaN");

    /* exhaustive round trip on ints around boundaries, and random bit patterns for doubles */
    int32_t ints[] = {0, 1, -1, 255, -256, 65535, 1 << 30, -(1 << 30), 2147483647, -2147483647 - 1};
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++)
        check(is_boxed(from_int(ints[i])) && tag_of(from_int(ints[i])) == T_INT && to_int(from_int(ints[i])) == ints[i],
              "int round trip");
    uint64_t s = 0xABCDEF;
    unsigned long numbers = 0, nans = 0;
    for (int i = 0; i < 100000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t bits = s ^ (s >> 29);
        double d;
        memcpy(&d, &bits, sizeof d);
        Value v = from_double(d);
        check(!is_boxed(v), "any double stays unboxed");
        if (d != d)
            nans++;
        else {
            check(to_double(v) == d, "double round trip");
            numbers++;
        }
    }
    printf("random doubles: %lu numbers, %lu nans, none boxed\n", numbers, nans);

    /* mixed arithmetic */
    Value r;
    check(add(from_int(40), from_int(2), &r) && !is_boxed(r) && to_double(r) == 42.0, "int+int");
    check(add(from_double(0.25), from_int(-3), &r) && to_double(r) == -2.75, "double+int");
    check(!add(from_bool(1), from_int(1), &r), "bool+int rejected");
    check(!add(make_null(), from_double(1.0), &r), "null+double rejected");
    printf("40+2=");
    add(from_int(40), from_int(2), &r);
    show(r);
    printf(" 0.25+(-3)=");
    add(from_double(0.25), from_int(-3), &r);
    show(r);
    printf("\n");

    /* summing a mixed array by dispatching on the tag */
    double total = 0;
    int skipped = 0;
    for (int i = 0; i < n; i++) {
        Value v = vals[i];
        if (!is_boxed(v)) {
            double d = to_double(v);
            if (d == d && !isinf(d) && fabs(d) < 1e100)
                total += d;
            else
                skipped++;
        } else if (tag_of(v) == T_INT) {
            total += (double)to_int(v);
        } else {
            skipped++;
        }
    }
    printf("finite numeric total=%.4f skipped=%d\n", total, skipped);
    return 0;
}
