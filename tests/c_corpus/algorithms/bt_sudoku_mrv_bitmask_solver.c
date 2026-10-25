/*
 * title: Sudoku backtracking with bitmask candidates and MRV
 * topic: algorithms
 * covers: constraint propagation, minimum remaining values, candidate bitmasks, solution counting, unsolvable detection
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int cell[81];
    unsigned row[9], col[9], box[9];
} Grid;

static long nodes;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int popcnt(unsigned v) {
    int c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

static int bit_index(unsigned v) {
    int i = 0;
    while (!((v >> i) & 1u))
        i++;
    return i;
}

static int box_of(int pos) {
    return (pos / 27) * 3 + (pos % 9) / 3;
}

static int load(Grid *g, const char *s) {
    memset(g, 0, sizeof *g);
    for (int i = 0; i < 81; i++) {
        int d = (s[i] >= '1' && s[i] <= '9') ? s[i] - '0' : 0;
        g->cell[i] = d;
        if (d) {
            unsigned b = 1u << d;
            int r = i / 9, c = i % 9, x = box_of(i);
            if ((g->row[r] | g->col[c] | g->box[x]) & b)
                return 0; /* clue conflicts with another clue */
            g->row[r] |= b;
            g->col[c] |= b;
            g->box[x] |= b;
        }
    }
    return 1;
}

/* Counts solutions up to `limit`, storing the first one in *first. Chooses the empty
 * cell with the fewest candidates each step (MRV), bailing out at zero. */
static int solve(Grid *g, int limit, Grid *first, int *have_first) {
    int best = -1, best_n = 10;
    unsigned best_mask = 0;
    for (int i = 0; i < 81; i++) {
        if (g->cell[i])
            continue;
        unsigned used = g->row[i / 9] | g->col[i % 9] | g->box[box_of(i)];
        unsigned cand = ~used & 0x3FEu;
        int n = popcnt(cand);
        if (n < best_n) {
            best_n = n;
            best = i;
            best_mask = cand;
            if (n <= 1)
                break;
        }
    }
    if (best < 0) {
        if (!*have_first) {
            *first = *g;
            *have_first = 1;
        }
        return 1;
    }
    int total = 0;
    int r = best / 9, c = best % 9, x = box_of(best);
    while (best_mask && total < limit) {
        unsigned b = best_mask & (0u - best_mask);
        best_mask ^= b;
        int d = bit_index(b);
        nodes++;
        g->cell[best] = d;
        g->row[r] |= b;
        g->col[c] |= b;
        g->box[x] |= b;
        total += solve(g, limit - total, first, have_first);
        g->cell[best] = 0;
        g->row[r] &= ~b;
        g->col[c] &= ~b;
        g->box[x] &= ~b;
    }
    return total;
}

static int valid_solution(const Grid *g) {
    for (int u = 0; u < 9; u++) {
        unsigned r = 0, c = 0, b = 0;
        for (int k = 0; k < 9; k++) {
            r |= 1u << g->cell[u * 9 + k];
            c |= 1u << g->cell[k * 9 + u];
            b |= 1u << g->cell[((u / 3) * 3 + k / 3) * 9 + (u % 3) * 3 + k % 3];
        }
        if (r != 0x3FEu || c != 0x3FEu || b != 0x3FEu)
            return 0;
    }
    return 1;
}

static void print_grid(const Grid *g) {
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++)
            putchar('0' + g->cell[r * 9 + c]);
        putchar('\n');
    }
}

int main(void) {
    struct {
        const char *name;
        const char *puzzle;
    } tests[] = {
        {"wikipedia",
         "530070000600195000098000060800060003400803001700020006060000280000419005000080079"},
        {"hard",
         "800000000003600000070090200050007000000045700000100030001000068008500010090000400"},
        {"almost-empty",
         "000000000000000000000000000000000000000000000000000000000000000000000000000000012"},
        {"two-solutions",
         "000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
        {"contradiction",
         "110000000000000000000000000000000000000000000000000000000000000000000000000000000"},
        {"no-solution",
         "123456780000000009000000000000000000000000000000000000000000000000000000000000000"},
    };
    Grid firsts[6];
    for (int t = 0; t < 6; t++) {
        Grid g, first;
        int have = 0;
        nodes = 0;
        if (!load(&g, tests[t].puzzle)) {
            printf("%-14s invalid clues\n", tests[t].name);
            continue;
        }
        int clues = 0;
        for (int i = 0; i < 81; i++)
            clues += g.cell[i] != 0;
        int n = solve(&g, 2, &first, &have);
        printf("%-14s clues=%2d solutions(<=2)=%d nodes=%ld\n", tests[t].name, clues, n, nodes);
        if (have) {
            check(valid_solution(&first), "solution satisfies all constraints");
            for (int i = 0; i < 81; i++)
                if (tests[t].puzzle[i] >= '1' && tests[t].puzzle[i] <= '9')
                    check(first.cell[i] == tests[t].puzzle[i] - '0', "clues preserved");
            firsts[t] = first;
        }
    }
    check(firsts[0].cell[2] == 4 && firsts[0].cell[80] == 9, "wikipedia known cells");
    printf("wikipedia solution:\n");
    print_grid(&firsts[0]);
    printf("hard solution row 1: ");
    for (int c = 0; c < 9; c++)
        putchar('0' + firsts[1].cell[c]);
    putchar('\n');
    return 0;
}
