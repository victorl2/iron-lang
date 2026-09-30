/*
 * title: Bytecode stack VM with GC roots from operand stack, frames and globals
 * topic: memory
 * covers: tagged values, pairs and small arrays in a slot heap, precise roots taken from the live operand stack and globals, allocation while operands are on the stack, cyclic arrays, call frames, stress mode with identical results, independent BFS oracle at every collection
 * deps: libc
 */
#define SEED 0x5EED001BULL
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__);    \
            exit(1);                                                          \
        }                                                                     \
    } while (0)


static long g_checks;

#define HEAP 96
#define MAXARR 6
#define NIL_V (-2)
#define MKI(n) ((n)*2 + 1)
#define IS_INT(v) (((v)&1) != 0)
#define INT_OF(v) (((v)-1) / 2)
#define IS_REF(v) ((v) >= 0 && ((v)&1) == 0)

typedef struct {
    int used, n;
    int v[MAXARR];
    unsigned char mark;
} Obj;

enum { PUSHI, PUSHNIL, LOAD, STORE, GLOAD, GSTORE, ADD, SUB, LT, JMP, JZ, JNIL, CONS, CAR, CDR, NEWARR, AGET, ASET,
       CALL, RET, ENTER, POP, RESULT, HALT };

static Obj heap[HEAP];
static int freelist, nfree;
static int stack[512], sp, fp;
static int globals[8];
static int frames_pc[64], frames_fp[64], nframes;
static int code[400], ncode;
static int stress;
static long steps, allocs, collections, peak_sp, peak_frames, freed_total;
static int results[16], nres;

static int mark_from(int v, int *work, int *nw) {
    if (!IS_REF(v) || heap[v / 2].mark)
        return 0;
    heap[v / 2].mark = 1;
    work[(*nw)++] = v / 2;
    return 1;
}

static void gc(void) {
    /* oracle: reachability by repeated relaxation, no worklist shared with the marker */
    unsigned char reach[HEAP];
    memset(reach, 0, sizeof reach);
    for (int i = 0; i < sp; i++)
        if (IS_REF(stack[i]))
            reach[stack[i] / 2] = 1;
    for (int i = 0; i < 8; i++)
        if (IS_REF(globals[i]))
            reach[globals[i] / 2] = 1;
    for (int changed = 1; changed;) {
        changed = 0;
        for (int o = 0; o < HEAP; o++)
            if (reach[o])
                for (int k = 0; k < heap[o].n; k++) {
                    int t = heap[o].v[k];
                    if (IS_REF(t) && !reach[t / 2]) {
                        reach[t / 2] = 1;
                        changed = 1;
                    }
                }
    }
    int work[HEAP], nw = 0;
    for (int i = 0; i < sp; i++)
        mark_from(stack[i], work, &nw);
    for (int i = 0; i < 8; i++)
        mark_from(globals[i], work, &nw);
    while (nw > 0) {
        int o = work[--nw];
        for (int k = 0; k < heap[o].n; k++)
            mark_from(heap[o].v[k], work, &nw);
    }
    freelist = -1;
    nfree = 0;
    for (int i = HEAP - 1; i >= 0; i--) {
        if (heap[i].used && !heap[i].mark) {
            heap[i].used = 0;
            freed_total++;
        }
        CHECK((heap[i].used != 0) == (reach[i] != 0));
        heap[i].mark = 0;
        if (!heap[i].used) {
            heap[i].v[0] = freelist;
            freelist = i;
            nfree++;
        }
    }
    collections++;
    g_checks++;
}

/* Called with all operands still on the stack, so a collection cannot free them. */
static int alloc_obj(int n) {
    if (stress || freelist < 0)
        gc();
    if (freelist < 0) {
        fprintf(stderr, "vm heap exhausted\n");
        exit(1);
    }
    int i = freelist;
    freelist = heap[i].v[0];
    nfree--;
    heap[i].used = 1;
    heap[i].n = n;
    for (int k = 0; k < MAXARR; k++)
        heap[i].v[k] = NIL_V;
    allocs++;
    return i;
}

static void run(void) {
    int pc = 0;
    sp = fp = nframes = 0;
    for (int i = 0; i < 8; i++)
        globals[i] = NIL_V;
    for (;;) {
        int op = code[pc++];
        steps++;
        if (sp > peak_sp)
            peak_sp = sp;
        switch (op) {
        case PUSHI: stack[sp++] = MKI(code[pc++]); break;
        case PUSHNIL: stack[sp++] = NIL_V; break;
        case LOAD: stack[sp] = stack[fp + code[pc++]]; sp++; break;
        case STORE: stack[fp + code[pc++]] = stack[--sp]; break;
        case GLOAD: stack[sp++] = globals[code[pc++]]; break;
        case GSTORE: globals[code[pc++]] = stack[--sp]; break;
        case ADD: CHECK(IS_INT(stack[sp - 1]) && IS_INT(stack[sp - 2])); stack[sp - 2] = MKI(INT_OF(stack[sp - 2]) + INT_OF(stack[sp - 1])); sp--; break;
        case SUB: CHECK(IS_INT(stack[sp - 1]) && IS_INT(stack[sp - 2])); stack[sp - 2] = MKI(INT_OF(stack[sp - 2]) - INT_OF(stack[sp - 1])); sp--; break;
        case LT: stack[sp - 2] = MKI(INT_OF(stack[sp - 2]) < INT_OF(stack[sp - 1])); sp--; break;
        case JMP: pc = code[pc]; break;
        case JZ: { int v = stack[--sp]; CHECK(IS_INT(v)); pc = INT_OF(v) == 0 ? code[pc] : pc + 1; break; }
        case JNIL: { int v = stack[--sp]; pc = v == NIL_V ? code[pc] : pc + 1; break; }
        case CONS: {
            int o = alloc_obj(2);
            heap[o].v[0] = stack[sp - 2];
            heap[o].v[1] = stack[sp - 1];
            sp -= 2;
            stack[sp++] = o * 2;
            break;
        }
        case CAR: CHECK(IS_REF(stack[sp - 1])); stack[sp - 1] = heap[stack[sp - 1] / 2].v[0]; break;
        case CDR: CHECK(IS_REF(stack[sp - 1])); stack[sp - 1] = heap[stack[sp - 1] / 2].v[1]; break;
        case NEWARR: { int o = alloc_obj(code[pc++]); stack[sp++] = o * 2; break; }
        case AGET: {
            int idx = INT_OF(stack[sp - 1]), a = stack[sp - 2];
            CHECK(IS_REF(a) && idx >= 0 && idx < heap[a / 2].n);
            sp -= 2;
            stack[sp++] = heap[a / 2].v[idx];
            break;
        }
        case ASET: {
            int val = stack[sp - 1], idx = INT_OF(stack[sp - 2]), a = stack[sp - 3];
            CHECK(IS_REF(a) && idx >= 0 && idx < heap[a / 2].n);
            heap[a / 2].v[idx] = val;
            sp -= 3;
            break;
        }
        case CALL: {
            int target = code[pc], nargs = code[pc + 1];
            frames_pc[nframes] = pc + 2;
            frames_fp[nframes] = fp;
            nframes++;
            if (nframes > peak_frames)
                peak_frames = nframes;
            fp = sp - nargs;
            pc = target;
            break;
        }
        case RET: {
            int v = stack[sp - 1];
            sp = fp;
            stack[sp++] = v;
            nframes--;
            pc = frames_pc[nframes];
            fp = frames_fp[nframes];
            break;
        }
        case ENTER: for (int k = code[pc++]; k > 0; k--) stack[sp++] = NIL_V; break;
        case POP: sp--; break;
        case RESULT: results[nres++] = INT_OF(stack[--sp]); break;
        case HALT: return;
        default: CHECK(0);
        }
    }
}

/* ---- tiny assembler ---- */
static int here(void) { return ncode; }
static void E(int op) { code[ncode++] = op; }
static void E1(int op, int a) { code[ncode++] = op; code[ncode++] = a; }
static void E2(int op, int a, int b) { code[ncode++] = op; code[ncode++] = a; code[ncode++] = b; }
static void patch(int at, int target) { code[at] = target; }

static void assemble(int n_list, int n_churn, int depth, int reps) {
    ncode = 0;
    E1(JMP, 0);
    int jmain = 1;
    /* build(d): the list d, d-1, ..., 1 */
    int build = here();
    E1(LOAD, 0);
    E1(JZ, 0);
    int jz1 = here() - 1;
    E1(LOAD, 0);
    E1(LOAD, 0);
    E1(PUSHI, 1);
    E(SUB);
    E2(CALL, build, 1);
    E(CONS);
    E(RET);
    patch(jz1, here());
    E(PUSHNIL);
    E(RET);
    /* len(l) */
    int len = here();
    E1(LOAD, 0);
    E1(JNIL, 0);
    int jn = here() - 1;
    E1(PUSHI, 1);
    E1(LOAD, 0);
    E(CDR);
    E2(CALL, len, 1);
    E(ADD);
    E(RET);
    patch(jn, here());
    E1(PUSHI, 0);
    E(RET);
    /* main */
    patch(jmain, here());
    E1(ENTER, 3); /* 0 = i, 1 = scratch, 2 = sum */
    /* part 1: list of n_list pairs in global 0, then summed */
    E1(PUSHI, 0); E1(STORE, 0);
    int l1 = here();
    E1(LOAD, 0); E1(PUSHI, n_list); E(LT); E1(JZ, 0);
    int j1 = here() - 1;
    E1(LOAD, 0); E1(GLOAD, 0); E(CONS); E1(GSTORE, 0);
    E1(LOAD, 0); E1(PUSHI, 1); E(ADD); E1(STORE, 0);
    E1(JMP, l1);
    patch(j1, here());
    E1(PUSHI, 0); E1(STORE, 2);
    E1(GLOAD, 0); E1(STORE, 1);
    int l2 = here();
    E1(LOAD, 1); E1(JNIL, 0);
    int j2 = here() - 1;
    E1(LOAD, 2); E1(LOAD, 1); E(CAR); E(ADD); E1(STORE, 2);
    E1(LOAD, 1); E(CDR); E1(STORE, 1);
    E1(JMP, l2);
    patch(j2, here());
    E1(LOAD, 2); E(RESULT);
    E(PUSHNIL);
    E1(GSTORE, 0); /* drop the list */
    /* part 2: churn self-referencing arrays, keep the last two */
    E1(PUSHI, 0); E1(STORE, 0);
    int l3 = here();
    E1(LOAD, 0); E1(PUSHI, n_churn); E(LT); E1(JZ, 0);
    int j3 = here() - 1;
    E1(NEWARR, 4); E1(STORE, 1);
    E1(LOAD, 1); E1(PUSHI, 0); E1(LOAD, 0); E(ASET);
    E1(LOAD, 1); E1(PUSHI, 1); E1(LOAD, 0); E(PUSHNIL); E(CONS); E(ASET);
    E1(LOAD, 1); E1(PUSHI, 2); E1(LOAD, 1); E(ASET);
    E1(GLOAD, 2); E1(GSTORE, 3); E1(LOAD, 1); E1(GSTORE, 2);
    E1(LOAD, 0); E1(PUSHI, 1); E(ADD); E1(STORE, 0);
    E1(JMP, l3);
    patch(j3, here());
    E1(GLOAD, 2); E1(PUSHI, 0); E(AGET); E1(GLOAD, 3); E1(PUSHI, 0); E(AGET); E(ADD); E(RESULT);
    E1(GLOAD, 2); E1(PUSHI, 2); E(AGET); E1(PUSHI, 1); E(AGET); E(CAR); E(RESULT); /* a[2] is a itself */
    /* part 3: repeated deep recursive construction */
    E1(PUSHI, 0); E1(STORE, 0);
    int l4 = here();
    E1(LOAD, 0); E1(PUSHI, reps); E(LT); E1(JZ, 0);
    int j4 = here() - 1;
    E1(PUSHI, depth); E2(CALL, build, 1); E1(GSTORE, 4);
    E1(GLOAD, 4); E2(CALL, len, 1); E1(STORE, 2);
    E1(LOAD, 0); E1(PUSHI, 1); E(ADD); E1(STORE, 0);
    E1(JMP, l4);
    patch(j4, here());
    E1(LOAD, 2); E(RESULT);
    E(HALT);
}

static void reset(void) {
    memset(heap, 0, sizeof heap);
    freelist = -1;
    nfree = 0;
    for (int i = HEAP - 1; i >= 0; i--) {
        heap[i].v[0] = freelist;
        freelist = i;
        nfree++;
    }
    nres = 0;
    steps = allocs = collections = freed_total = peak_sp = peak_frames = 0;
}

int main(void) {
    assemble(50, 300, 40, 25);
    reset();
    run();
    int normal[16], n = nres;
    memcpy(normal, results, sizeof normal);
    long st = steps, al = allocs, co = collections, fr = freed_total, ps = peak_sp, pf = peak_frames;
    reset();
    stress = 1;
    run();
    long co2 = collections;
    CHECK(nres == n && memcmp(normal, results, sizeof normal) == 0);
    CHECK(normal[0] == 49 * 50 / 2 && normal[1] == 299 + 298 && normal[2] == 300 - 1 && normal[3] == 40);
    printf("results: list sum %d, churn %d, self cycle %d, recursion length %d\n", normal[0], normal[1], normal[2],
           normal[3]);
    printf("instructions: %ld, allocations: %ld\n", st, al);
    printf("collections: %ld (heap %d objects), objects freed: %ld\n", co, HEAP, fr);
    printf("peak operand stack: %ld, peak call depth: %ld\n", ps, pf);
    printf("stress run collections: %ld, results identical\n", co2);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
