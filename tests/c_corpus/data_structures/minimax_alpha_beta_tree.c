/*
 * title: Minimax game tree with alpha-beta pruning
 * topic: data_structures
 * covers: game tree, minimax, negamax, alpha-beta, move ordering, principal variation, transposition table, tic-tac-toe, nim
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static long nodes;

static uint64_t rs = 0x61A3E7EEULL * 977;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* ============ tic-tac-toe ============ */
typedef struct { signed char b[9]; } Board; /* 0 empty, 1 X, -1 O */
static const int lines[8][3] = { {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6} };
static int winner(const Board *s) {
    for (int i = 0; i < 8; i++) { int a = s->b[lines[i][0]]; if (a && a == s->b[lines[i][1]] && a == s->b[lines[i][2]]) return a; }
    return 0;
}
static int full(const Board *s) { for (int i = 0; i < 9; i++) if (!s->b[i]) return 0; return 1; }
/* value from X's point of view: +1 X wins, -1 O wins, 0 draw; max player = X */
static int minimax(Board *s, int player) {
    nodes++;
    int w = winner(s);
    if (w) return w;
    if (full(s)) return 0;
    int best = player == 1 ? -2 : 2;
    for (int i = 0; i < 9; i++) if (!s->b[i]) {
        s->b[i] = (signed char)player;
        int v = minimax(s, -player);
        s->b[i] = 0;
        if (player == 1 ? v > best : v < best) best = v;
    }
    return best;
}
static int negamax_ab(Board *s, int player, int alpha, int beta, const int *order) {
    nodes++;
    int w = winner(s);
    if (w) return w * player;
    if (full(s)) return 0;
    int best = -2;
    for (int k = 0; k < 9; k++) {
        int i = order ? order[k] : k;
        if (s->b[i]) continue;
        s->b[i] = (signed char)player;
        int v = -negamax_ab(s, -player, -beta, -alpha, order);
        s->b[i] = 0;
        if (v > best) best = v;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}
/* transposition table over 3^9 positions */
static signed char tt[19683]; static unsigned char tt_has[19683]; static long tt_hits;
static int code(const Board *s) { int c = 0; for (int i = 0; i < 9; i++) c = c * 3 + (s->b[i] + 1); return c; }
static int minimax_tt(Board *s, int player) {
    nodes++;
    int c = code(s);
    if (tt_has[c]) { tt_hits++; return tt[c]; }
    int w = winner(s), v;
    if (w) v = w;
    else if (full(s)) v = 0;
    else {
        v = player == 1 ? -2 : 2;
        for (int i = 0; i < 9; i++) if (!s->b[i]) {
            s->b[i] = (signed char)player; int x = minimax_tt(s, -player); s->b[i] = 0;
            if (player == 1 ? x > v : x < v) v = x;
        }
    }
    tt[c] = (signed char)v; tt_has[c] = 1;
    return v;
}
static const char *pvname(int v) { return v > 0 ? "X wins" : (v < 0 ? "O wins" : "draw"); }

/* ============ synthetic uniform game tree with hashed leaf values ============ */
static int B = 4, D = 8;
static uint32_t mixh(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return (uint32_t)x; }
static int leafval(uint64_t path) { return (int)(mixh(path * 2654435761ULL + 12345) % 201) - 100; }
static int mm_syn(uint64_t path, int depth, int maxp) {
    nodes++;
    if (depth == D) return leafval(path);
    int best = maxp ? -1000 : 1000;
    for (int m = 0; m < B; m++) {
        int v = mm_syn(path * 8 + (uint64_t)m + 1, depth + 1, !maxp);
        if (maxp ? v > best : v < best) best = v;
    }
    return best;
}
static int ab_syn(uint64_t path, int depth, int alpha, int beta, int maxp, int *pvmove) {
    nodes++;
    if (depth == D) return leafval(path);
    int best = maxp ? -1000 : 1000;
    for (int m = 0; m < B; m++) {
        int v = ab_syn(path * 8 + (uint64_t)m + 1, depth + 1, alpha, beta, !maxp, NULL);
        if (maxp ? v > best : v < best) { best = v; if (pvmove) *pvmove = m; }
        if (maxp) { if (best > alpha) alpha = best; } else if (best < beta) beta = best;
        if (alpha >= beta) break;
    }
    return best;
}
/* alpha-beta with the best child first (children ordered by their exact values, an oracle order) */
static int ab_syn_ordered(uint64_t path, int depth, int alpha, int beta, int maxp) {
    nodes++;
    if (depth == D) return leafval(path);
    int vals[8], ord[8];
    long saved = nodes;
    for (int m = 0; m < B; m++) { vals[m] = mm_syn(path * 8 + (uint64_t)m + 1, depth + 1, !maxp); ord[m] = m; }
    nodes = saved; /* the ordering oracle is not counted */
    for (int i = 1; i < B; i++) { int o = ord[i], j = i - 1; while (j >= 0 && (maxp ? vals[ord[j]] < vals[o] : vals[ord[j]] > vals[o])) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = o; }
    int best = maxp ? -1000 : 1000;
    for (int k = 0; k < B; k++) {
        int m = ord[k];
        int v = ab_syn_ordered(path * 8 + (uint64_t)m + 1, depth + 1, alpha, beta, !maxp);
        if (maxp ? v > best : v < best) best = v;
        if (maxp) { if (best > alpha) alpha = best; } else if (best < beta) beta = best;
        if (alpha >= beta) break;
    }
    return best;
}

/* ============ nim (subtraction game): losing positions are multiples of (k+1) ============ */
static signed char nim_memo[200]; static int nim_k = 3;
static int nim_win(int n) { /* 1 if the player to move wins */
    if (n == 0) return 0;
    if (nim_memo[n]) return nim_memo[n] > 0;
    int w = 0;
    for (int t = 1; t <= nim_k && t <= n && !w; t++) if (!nim_win(n - t)) w = 1;
    nim_memo[n] = w ? 1 : -1;
    return w;
}

int main(void) {
    /* tic-tac-toe: exact game value and node counts */
    Board s; memset(&s, 0, sizeof s);
    nodes = 0; int v1 = minimax(&s, 1); long n1 = nodes;
    nodes = 0; int v2 = negamax_ab(&s, 1, -2, 2, NULL); long n2 = nodes;
    static const int centre_first[9] = { 4, 0, 2, 6, 8, 1, 3, 5, 7 };
    nodes = 0; int v3 = negamax_ab(&s, 1, -2, 2, centre_first); long n3 = nodes;
    nodes = 0; int v4 = minimax_tt(&s, 1); long n4 = nodes;
    check(v1 == 0 && v2 == v1 && v3 == v1 && v4 == v1, "tic-tac-toe is a draw under all searches");
    check(n1 == 549946, "known full-tree node count of tic-tac-toe");
    printf("tic-tac-toe: value %d (%s)\n", v1, pvname(v1));
    printf("  minimax %ld nodes, alpha-beta %ld nodes, alpha-beta centre-first %ld nodes, memoized %ld nodes (%ld table hits)\n", n1, n2, n3, n4, tt_hits);
    check(n2 < n1 && n3 <= n2 * 2 && n4 < n2, "pruning and memoization reduce work");
    /* every first move value */
    printf("  first-move values:");
    for (int m = 0; m < 9; m++) { Board c = s; c.b[m] = 1; nodes = 0; int a = minimax(&c, -1); int b = -negamax_ab(&c, -1, -2, 2, NULL); check(a == b, "root move agreement"); printf(" %d", a); }
    printf("\n");
    /* forced win detection: X to move with two in a row */
    Board w = s; w.b[0] = 1; w.b[1] = 1; w.b[4] = -1; w.b[8] = -1;
    int best = -1, bv = -2;
    for (int m = 0; m < 9; m++) if (!w.b[m]) { Board c = w; c.b[m] = 1; int v = minimax(&c, -1); if (v > bv) { bv = v; best = m; } }
    check(bv == 1 && best == 2, "X completes the top row");
    printf("  position with X to move and a top-row threat: best move %d value %d\n", best, bv);

    /* random-ish synthetic trees of increasing size */
    for (int depth = 4; depth <= 8; depth += 2) {
        D = depth; B = 4;
        nodes = 0; int a = mm_syn(0, 0, 1); long full_n = nodes;
        nodes = 0; int pv = -1; int b = ab_syn(0, 0, -1000, 1000, 1, &pv); long ab_n = nodes;
        nodes = 0; int c = ab_syn_ordered(0, 0, -1000, 1000, 1); long ord_n = nodes;
        check(a == b && a == c, "alpha-beta returns the minimax value");
        check(ab_n <= full_n && ord_n <= ab_n, "node counts ordered");
        printf("synthetic b=4 d=%d: value %4d, best root move %d, minimax %6ld nodes, alpha-beta %6ld, ordered %6ld\n", depth, a, pv, full_n, ab_n, ord_n);
    }
    /* many random subtrees: alpha-beta always equals minimax */
    D = 6; B = 3; int mism = 0;
    for (int t = 0; t < 200; t++) {
        uint64_t base = rnd(); int maxp = (int)(rnd() & 1);
        int a = mm_syn(base * 8 + 1, 0, maxp), b = ab_syn(base * 8 + 1, 0, -1000, 1000, maxp, NULL);
        if (a != b) mism++;
    }
    check(mism == 0, "alpha-beta equals minimax on 200 random trees");
    /* subtraction game */
    int losing = 0;
    for (int k = 2; k <= 5; k++) {
        nim_k = k; memset(nim_memo, 0, sizeof nim_memo);
        for (int n = 0; n < 150; n++) { check(nim_win(n) == (n % (k + 1) != 0), "subtraction game theory"); if (!nim_win(n)) losing++; }
    }
    printf("subtraction games k=2..5 over n<150: %d losing positions, all multiples of k+1\n", losing);
    return 0;
}
