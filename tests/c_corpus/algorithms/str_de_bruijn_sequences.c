/*
 * title: De Bruijn sequences by three constructions
 * topic: algorithms
 * covers: de Bruijn sequence, Lyndon words (FKM), greedy prefer-largest, Eulerian circuit, cyclic window verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* De Bruijn sequences B(k, n) by three methods: FKM (Lyndon words), the prefer-largest
 * greedy walk, and an Eulerian circuit in the de Bruijn graph (Hierholzer). */
static int K, Nn;
static int seq[1 << 16], slen;
static int a_buf[32];

static void fkm(int t, int p) {
    if (t > Nn) {
        if (Nn % p == 0)
            for (int i = 1; i <= p; i++)
                seq[slen++] = a_buf[i];
        return;
    }
    a_buf[t] = a_buf[t - p];
    fkm(t + 1, p);
    for (int j = a_buf[t - p] + 1; j < K; j++) {
        a_buf[t] = j;
        fkm(t + 1, t);
    }
}

static int method_fkm(int k, int n, int *out) {
    K = k;
    Nn = n;
    slen = 0;
    memset(a_buf, 0, sizeof a_buf);
    fkm(1, 1);
    memcpy(out, seq, sizeof(int) * (size_t)slen);
    return slen;
}

/* prefer-largest: start with n zeros, append the largest digit that creates a new window. */
static int method_greedy(int k, int n, int *out) {
    int total = 1;
    for (int i = 0; i < n; i++)
        total *= k;
    unsigned char *seen = calloc((size_t)total, 1);
    int len = 0, win = 0;
    for (int i = 0; i < n; i++)
        out[len++] = 0;
    seen[0] = 1;
    int top = total / k;
    for (;;) {
        int d;
        for (d = k - 1; d >= 0; d--) {
            int nw = (win % top) * k + d;
            if (!seen[nw]) {
                seen[nw] = 1;
                win = nw;
                out[len++] = d;
                break;
            }
        }
        if (d < 0)
            break;
    }
    free(seen);
    /* result has total + n - 1 symbols; a cyclic sequence drops the last n-1 */
    return len - (n - 1);
}

/* Eulerian circuit over (n-1)-symbol nodes, edges labelled by digit. */
static int method_euler(int k, int n, int *out) {
    int nodes = 1;
    for (int i = 0; i < n - 1; i++)
        nodes *= k;
    int *next_digit = calloc((size_t)nodes, sizeof(int));
    int *stack = malloc(sizeof(int) * (size_t)(nodes * k + 2));
    int *lab = malloc(sizeof(int) * (size_t)(nodes * k + 2));
    int *circuit = malloc(sizeof(int) * (size_t)(nodes * k + 2));
    int sp = 0, cl = 0;
    stack[sp] = 0;
    lab[sp++] = -1;
    while (sp > 0) {
        int v = stack[sp - 1];
        if (next_digit[v] < k) {
            int d = next_digit[v]++;
            stack[sp] = (v * k + d) % nodes;
            lab[sp++] = d;
        } else {
            circuit[cl++] = lab[sp - 1];
            sp--;
        }
    }
    /* circuit lists edge labels in reverse order, last entry is the -1 marker */
    int len = 0;
    for (int i = cl - 2; i >= 0; i--)
        out[len++] = circuit[i];
    free(next_digit);
    free(stack);
    free(lab);
    free(circuit);
    return len;
}

/* verify every length-n window of the cyclic sequence appears exactly once */
static int verify(const int *s, int len, int k, int n) {
    int total = 1;
    for (int i = 0; i < n; i++)
        total *= k;
    if (len != total)
        return 0;
    unsigned char *seen = calloc((size_t)total, 1);
    int ok = 1;
    for (int i = 0; i < len && ok; i++) {
        int w = 0;
        for (int j = 0; j < n; j++)
            w = w * k + s[(i + j) % len];
        if (seen[w])
            ok = 0;
        seen[w] = 1;
    }
    free(seen);
    return ok;
}

static void print_seq(const int *s, int len) {
    for (int i = 0; i < len && i < 40; i++)
        putchar('0' + s[i]);
    if (len > 40)
        printf("... (%d symbols)", len);
}

int main(void) {
    static int out[1 << 16];
    int cases[][2] = {{2, 3}, {2, 4}, {3, 2}, {3, 3}, {4, 3}, {2, 10}, {5, 3}};
    for (int c = 0; c < 7; c++) {
        int k = cases[c][0], n = cases[c][1];
        int expected = 1;
        for (int i = 0; i < n; i++)
            expected *= k;
        int l1 = method_fkm(k, n, out);
        check(verify(out, l1, k, n), "FKM valid");
        printf("B(%d,%d) FKM   : ", k, n);
        print_seq(out, l1);
        printf("\n");
        int l2 = method_greedy(k, n, out);
        check(verify(out, l2, k, n), "greedy valid");
        printf("B(%d,%d) greedy: ", k, n);
        print_seq(out, l2);
        printf("\n");
        int l3 = method_euler(k, n, out);
        check(l3 == expected && verify(out, l3, k, n), "Euler valid");
        printf("B(%d,%d) euler : ", k, n);
        print_seq(out, l3);
        printf("\n");
        check(l1 == expected, "length");
    }
    return 0;
}
