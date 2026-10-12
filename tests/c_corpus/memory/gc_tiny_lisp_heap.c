/*
 * title: Tiny Lisp interpreter on a garbage-collected cons heap
 * topic: memory
 * covers: tagged values (fixnum, symbol, special, cell reference), cons cells and closures in a fixed heap, reader and evaluator with an explicit root stack, mark-sweep triggered by allocation, GC stress mode with identical results, independent BFS reachability oracle at every collection
 * deps: libc
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEED 0x5EED001AULL
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

#define HEAP 640

typedef int Val;
/* low two bits: 0 cell reference, 1 fixnum, 2 symbol, 3 special */
#define MKINT(n) ((Val)((n)*4 + 1))
#define ISINT(v) (((v)&3) == 1)
#define INTOF(v) (((v)-1) / 4)
#define ISREF(v) (((v)&3) == 0)
#define MKSYM(i) ((Val)((i)*4 + 2))
#define ISSYM(v) (((v)&3) == 2)
#define SYMOF(v) ((v) / 4)
#define SPECIAL(i) ((Val)((i)*4 + 3))
#define NIL SPECIAL(0)
#define TRUE_ SPECIAL(1)
#define FALSE_ SPECIAL(2)
#define PRIM(i) SPECIAL(8 + (i))
#define ISPRIM(v) (((v)&3) == 3 && (v) / 4 >= 8)
#define PRIMOF(v) ((v) / 4 - 8)

enum { T_FREE, T_CONS, T_CLOSURE };
typedef struct {
    int tag;
    Val a, b;
    unsigned char mark;
} Cell;

static Cell *heap;
static int ncells, freelist, nfree;
static Val rstack[4096];
static int rsp;
static Val genv = NIL;
static int stress; /* collect on every allocation */
static long gc_count, cells_freed, allocs, peak_live;

#define PUSH(v) (rstack[rsp++] = (v))
#define POP() (void)(--rsp)

static char symnames[64][12];
static int nsyms;
static int intern(const char *s, int n) {
    for (int i = 0; i < nsyms; i++)
        if ((int)strlen(symnames[i]) == n && strncmp(symnames[i], s, (size_t)n) == 0)
            return i;
    if (nsyms == 64 || n >= 12) {
        fprintf(stderr, "symbol table full\n");
        exit(1);
    }
    memcpy(symnames[nsyms], s, (size_t)n);
    symnames[nsyms][n] = 0;
    return nsyms++;
}

static void fail(const char *m) {
    fprintf(stderr, "lisp error: %s\n", m);
    exit(1);
}

/* ---- collector ---- */
static void mark_val(Val v) {
    while (ISREF(v)) {
        int i = v / 4;
        if (heap[i].mark)
            return;
        heap[i].mark = 1;
        mark_val(heap[i].a);
        v = heap[i].b;
    }
}

static int oracle_reach(unsigned char *seen) { /* breadth-first, no recursion, independent of mark_val */
    int *q = malloc((size_t)ncells * sizeof(int));
    int qh = 0, qt = 0, n = 0;
    memset(seen, 0, (size_t)ncells);
    Val starts[4100];
    int ns = 0;
    starts[ns++] = genv;
    for (int i = 0; i < rsp; i++)
        starts[ns++] = rstack[i];
    for (int i = 0; i < ns || qh < qt;) {
        Val v;
        if (i < ns)
            v = starts[i++];
        else {
            Cell *c = &heap[q[qh++]];
            for (int k = 0; k < 2; k++) {
                Val w = k ? c->b : c->a;
                if (ISREF(w) && !seen[w / 4]) {
                    seen[w / 4] = 1;
                    q[qt++] = w / 4;
                    n++;
                }
            }
            continue;
        }
        if (ISREF(v) && !seen[v / 4]) {
            seen[v / 4] = 1;
            q[qt++] = v / 4;
            n++;
        }
    }
    free(q);
    return n;
}

static void gc(void) {
    unsigned char *expect = malloc((size_t)ncells);
    int want = oracle_reach(expect);
    mark_val(genv);
    for (int i = 0; i < rsp; i++)
        mark_val(rstack[i]);
    int got = 0;
    freelist = -1;
    nfree = 0;
    for (int i = ncells - 1; i >= 0; i--) {
        if (heap[i].tag != T_FREE && !heap[i].mark) {
            heap[i].tag = T_FREE;
            cells_freed++;
        }
        CHECK((heap[i].tag != T_FREE) == (expect[i] != 0)); /* survivors are exactly the reachable cells */
        got += heap[i].mark;
        heap[i].mark = 0;
        if (heap[i].tag == T_FREE) {
            heap[i].b = freelist;
            freelist = i * 4;
            nfree++;
        }
    }
    CHECK(got == want);
    if (got > peak_live)
        peak_live = got;
    free(expect);
    gc_count++;
    g_checks++;
}

static Val alloc_cell(int tag, Val a, Val b) {
    PUSH(a);
    PUSH(b); /* the operands are roots while a collection may run */
    if (stress || freelist < 0)
        gc();
    if (freelist < 0)
        fail("out of cells");
    rsp -= 2;
    b = rstack[rsp + 1];
    a = rstack[rsp];
    int i = freelist / 4;
    freelist = heap[i].b;
    nfree--;
    heap[i].tag = tag;
    heap[i].a = a;
    heap[i].b = b;
    allocs++;
    return (Val)(i * 4);
}
static Val cons(Val a, Val b) { return alloc_cell(T_CONS, a, b); }
static Val car(Val v) {
    if (!ISREF(v) || heap[v / 4].tag != T_CONS)
        fail("car of non-pair");
    return heap[v / 4].a;
}
static Val cdr(Val v) {
    if (!ISREF(v) || heap[v / 4].tag != T_CONS)
        fail("cdr of non-pair");
    return heap[v / 4].b;
}

/* ---- reader: builds lists with every partial result on the root stack ---- */
static const char *src;
static void skip_ws(void) {
    while (*src == ' ' || *src == '\n')
        src++;
}
static Val read_expr(void) {
    skip_ws();
    if (*src == '\'') {
        src++;
        int base = rsp;
        Val q = read_expr();
        PUSH(q);
        Val tail = cons(q, NIL);
        PUSH(tail);
        Val r = cons(MKSYM(intern("quote", 5)), tail);
        rsp = base;
        return r;
    }
    if (*src == '(') {
        src++;
        int base = rsp;
        for (;;) {
            skip_ws();
            if (*src == ')') {
                src++;
                break;
            }
            Val e = read_expr();
            PUSH(e);
        }
        Val l = NIL;
        for (int i = rsp - 1; i >= base; i--) {
            PUSH(l);
            l = cons(rstack[i], l);
            POP();
        }
        rsp = base;
        return l;
    }
    const char *s = src;
    while (*src && *src != ' ' && *src != '\n' && *src != ')' && *src != '(')
        src++;
    int n = (int)(src - s);
    if (isdigit((unsigned char)*s) || (*s == '-' && n > 1)) {
        int v = 0;
        for (int i = (*s == '-'); i < n; i++)
            v = v * 10 + (s[i] - '0');
        return MKINT(*s == '-' ? -v : v);
    }
    return MKSYM(intern(s, n));
}

/* ---- evaluator ---- */
enum { P_ADD, P_SUB, P_MUL, P_LT, P_EQ, P_CONS, P_CAR, P_CDR, P_NULLP, NPRIM };
static const char *primnames[NPRIM] = {"+", "-", "*", "<", "=", "cons", "car", "cdr", "null?"};
static int S_QUOTE, S_IF, S_DEFINE, S_LAMBDA, S_BEGIN, S_LET;

static Val lookup(Val sym, Val env) {
    for (Val e = env; e != NIL; e = cdr(e))
        if (car(car(e)) == sym)
            return cdr(car(e));
    for (Val e = genv; e != NIL; e = cdr(e))
        if (car(car(e)) == sym)
            return cdr(car(e));
    fail("unbound variable");
    return NIL;
}

static Val eval(Val x, Val env);

static Val apply_prim(int p, Val args) {
    Val a = car(args), b = NIL;
    if (p != P_CAR && p != P_CDR && p != P_NULLP)
        b = car(cdr(args));
    switch (p) {
    case P_ADD: return MKINT(INTOF(a) + INTOF(b));
    case P_SUB: return MKINT(INTOF(a) - INTOF(b));
    case P_MUL: return MKINT(INTOF(a) * INTOF(b));
    case P_LT: return INTOF(a) < INTOF(b) ? TRUE_ : FALSE_;
    case P_EQ: return a == b ? TRUE_ : FALSE_;
    case P_CONS: return cons(a, b);
    case P_CAR: return car(a);
    case P_CDR: return cdr(a);
    default: return a == NIL ? TRUE_ : FALSE_;
    }
}

static Val evlis(Val args, Val env) {
    if (args == NIL)
        return NIL;
    int base = rsp;
    PUSH(args);
    PUSH(env);
    Val v = eval(car(args), env);
    PUSH(v);
    Val rest = evlis(cdr(args), env);
    PUSH(rest);
    Val r = cons(v, rest);
    rsp = base;
    return r;
}

static Val eval(Val x, Val env) {
    int base = rsp;
    PUSH(x);
    PUSH(env);
    Val result;
    if (ISSYM(x)) {
        result = lookup(x, env);
    } else if (!ISREF(x)) {
        result = x;
    } else {
        Val head = car(x);
        if (head == MKSYM(S_QUOTE)) {
            result = car(cdr(x));
        } else if (head == MKSYM(S_IF)) {
            Val c = eval(car(cdr(x)), env);
            Val branch = c != FALSE_ ? car(cdr(cdr(x))) : car(cdr(cdr(cdr(x))));
            result = eval(branch, env);
        } else if (head == MKSYM(S_DEFINE)) {
            Val target = car(cdr(x)), body;
            Val name = target;
            if (ISREF(target)) { /* (define (f a b) body) */
                name = car(target);
                Val lam_tail = cons(cdr(target), cdr(cdr(x)));
                PUSH(lam_tail);
                body = alloc_cell(T_CLOSURE, lam_tail, NIL);
            } else {
                body = eval(car(cdr(cdr(x))), env);
            }
            PUSH(body);
            Val pair = cons(name, body);
            PUSH(pair);
            genv = cons(pair, genv);
            result = name;
        } else if (head == MKSYM(S_LAMBDA)) {
            result = alloc_cell(T_CLOSURE, cdr(x), env);
        } else if (head == MKSYM(S_BEGIN)) {
            result = NIL;
            for (Val e = cdr(x); e != NIL; e = cdr(e))
                result = eval(car(e), env);
        } else if (head == MKSYM(S_LET)) { /* (let ((n e) ...) body) */
            Val ne = env;
            PUSH(ne);
            int slot = rsp - 1;
            for (Val b = car(cdr(x)); b != NIL; b = cdr(b)) {
                Val v = eval(car(cdr(car(b))), env);
                PUSH(v);
                Val pair = cons(car(car(b)), v);
                PUSH(pair);
                rstack[slot] = cons(pair, rstack[slot]);
            }
            result = eval(car(cdr(cdr(x))), rstack[slot]);
        } else {
            Val f = eval(head, env);
            PUSH(f);
            Val args = evlis(cdr(x), env);
            PUSH(args);
            if (ISPRIM(f)) {
                result = apply_prim(PRIMOF(f), args);
            } else {
                if (!ISREF(f) || heap[f / 4].tag != T_CLOSURE)
                    fail("not a function");
                Val params = car(heap[f / 4].a), body = cdr(heap[f / 4].a);
                Val ne = heap[f / 4].b;
                PUSH(ne);
                int slot = rsp - 1;
                Val a = args;
                for (Val p = params; p != NIL; p = cdr(p), a = cdr(a)) {
                    Val pair = cons(car(p), car(a));
                    PUSH(pair);
                    rstack[slot] = cons(pair, rstack[slot]);
                }
                result = NIL;
                for (Val e = body; e != NIL; e = cdr(e))
                    result = eval(car(e), rstack[slot]);
            }
        }
    }
    rsp = base;
    return result;
}

static const char *program[] = {
    "(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))",
    "(define (range a b) (if (< a b) (cons a (range (+ a 1) b)) '()))",
    "(define (map f l) (if (null? l) '() (cons (f (car l)) (map f (cdr l)))))",
    "(define (sum l) (if (null? l) 0 (+ (car l) (sum (cdr l)))))",
    "(define (len l) (if (null? l) 0 (+ 1 (len (cdr l)))))",
    "(define (rev l acc) (if (null? l) acc (rev (cdr l) (cons (car l) acc))))",
    "(define (make-adder n) (lambda (x) (+ x n)))",
    "(define (ack m n) (if (= m 0) (+ n 1) (if (= n 0) (ack (- m 1) 1) (ack (- m 1) (ack m (- n 1))))))",
    "(define (churn n) (if (= n 0) 0 (begin (range 0 12) (churn (- n 1)))))",
    "(define (compose f g) (lambda (x) (f (g x))))",
};
static const char *queries[] = {
    "(fib 12)",
    "(sum (map (lambda (x) (* x x)) (range 0 25)))",
    "(len (rev (range 0 40) '()))",
    "((make-adder 5) 10)",
    "(ack 2 3)",
    "(churn 40)",
    "(let ((a 6) (b 7)) (* a b))",
    "((compose (make-adder 1) (make-adder 20)) 1)",
    "(car (rev (range 0 30) '()))",
};
static const int answers[] = {144, 4900, 40, 15, 9, 0, 42, 22, 29};

static void run_all(int nq, int *results) {
    for (size_t i = 0; i < sizeof program / sizeof program[0]; i++) {
        src = program[i];
        Val e = read_expr();
        PUSH(e);
        eval(e, NIL);
        POP();
    }
    for (int i = 0; i < nq; i++) {
        src = queries[i];
        Val e = read_expr();
        PUSH(e);
        Val r = eval(e, NIL);
        POP();
        CHECK(ISINT(r));
        results[i] = INTOF(r);
    }
}

static void reset_heap(int n) {
    free(heap);
    ncells = n;
    heap = calloc((size_t)n, sizeof(Cell));
    CHECK(heap != NULL);
    freelist = -1;
    nfree = 0;
    for (int i = n - 1; i >= 0; i--) {
        heap[i].b = freelist;
        freelist = i * 4;
        nfree++;
    }
    rsp = 0;
    genv = NIL;
    gc_count = cells_freed = allocs = peak_live = 0;
}

static void install_prims(void) {
    for (int i = 0; i < NPRIM; i++) {
        Val name = MKSYM(intern(primnames[i], (int)strlen(primnames[i])));
        Val pair = cons(name, PRIM(i));
        PUSH(pair);
        genv = cons(pair, genv);
        POP();
    }
}

int main(void) {
    S_QUOTE = intern("quote", 5);
    S_IF = intern("if", 2);
    S_DEFINE = intern("define", 6);
    S_LAMBDA = intern("lambda", 6);
    S_BEGIN = intern("begin", 5);
    S_LET = intern("let", 3);
    int nq = (int)(sizeof queries / sizeof queries[0]);
    int normal[16], stressed[16];
    reset_heap(HEAP);
    install_prims();
    run_all(nq, normal);
    long g1 = gc_count, f1 = cells_freed, a1 = allocs, pk = peak_live;
    reset_heap(HEAP);
    stress = 1;
    install_prims();
    run_all(nq, stressed);
    long g2 = gc_count;
    for (int i = 0; i < nq; i++) {
        CHECK(normal[i] == answers[i]);
        CHECK(stressed[i] == normal[i]);
        printf("%-52s => %d\n", queries[i], normal[i]);
    }
    printf("heap %d cells: %ld allocations, %ld collections, %ld cells reclaimed, peak live %ld\n", HEAP, a1, g1, f1, pk);
    printf("stress mode (collect on every allocation): %ld collections\n", g2);
    printf("oracle checks: %ld\n", g_checks);
    free(heap);
    return 0;
}
