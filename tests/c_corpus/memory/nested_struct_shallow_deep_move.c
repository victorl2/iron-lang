/*
 * title: Nested struct shallow vs deep copy vs move
 * topic: memory
 * covers: struct copy aliasing, deep copy, move with source reset, ownership of nested heap members, aliasing detection
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    int *scores;
    int nscores;
} Student;

typedef struct {
    char *title;
    Student *students;
    int n;
    Student best; /* embedded by value, but it owns heap members too */
} Course;

static int live_allocs;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    check(p != NULL, "alloc");
    live_allocs++;
    return p;
}
static void xfree(void *p) {
    if (p) {
        free(p);
        live_allocs--;
    }
}

static char *dupstr(const char *s) {
    char *d = xmalloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

static Student student_make(const char *name, int base, int n) {
    Student s;
    s.name = dupstr(name);
    s.scores = xmalloc((size_t)n * sizeof(int));
    s.nscores = n;
    for (int i = 0; i < n; i++)
        s.scores[i] = base + i * 3;
    return s;
}

static Student student_clone(const Student *s) {
    Student c;
    c.name = dupstr(s->name);
    c.scores = xmalloc((size_t)s->nscores * sizeof(int));
    memcpy(c.scores, s->scores, (size_t)s->nscores * sizeof(int));
    c.nscores = s->nscores;
    return c;
}

static void student_free(Student *s) {
    xfree(s->name);
    xfree(s->scores);
    memset(s, 0, sizeof *s);
}

static Course course_make(void) {
    Course c;
    c.title = dupstr("Systems");
    c.n = 3;
    c.students = xmalloc(3 * sizeof(Student));
    c.students[0] = student_make("ada", 70, 3);
    c.students[1] = student_make("bob", 60, 4);
    c.students[2] = student_make("cy", 80, 2);
    c.best = student_clone(&c.students[2]);
    return c;
}

static Course course_clone(const Course *src) {
    Course c;
    c.title = dupstr(src->title);
    c.n = src->n;
    c.students = xmalloc((size_t)src->n * sizeof(Student));
    for (int i = 0; i < src->n; i++)
        c.students[i] = student_clone(&src->students[i]);
    c.best = student_clone(&src->best);
    return c;
}

static void course_free(Course *c) {
    for (int i = 0; i < c->n; i++)
        student_free(&c->students[i]);
    xfree(c->students);
    xfree(c->title);
    student_free(&c->best);
    memset(c, 0, sizeof *c);
}

static Course course_move(Course *src) {
    Course c = *src;
    memset(src, 0, sizeof *src);
    return c;
}

static int shares_memory(const Course *a, const Course *b) {
    if (a->title == b->title || a->students == b->students || a->best.name == b->best.name)
        return 1;
    for (int i = 0; i < a->n && i < b->n; i++)
        if (a->students[i].name == b->students[i].name ||
            a->students[i].scores == b->students[i].scores)
            return 1;
    return 0;
}

static void describe(const char *tag, const Course *c) {
    printf("%s: %s [", tag, c->title);
    for (int i = 0; i < c->n; i++)
        printf("%s%s=%d", i ? " " : "", c->students[i].name, c->students[i].scores[0]);
    printf("] best=%s/%d\n", c->best.name, c->best.scores[c->best.nscores - 1]);
}

int main(void) {
    Course a = course_make();
    int base_allocs = live_allocs;
    printf("allocations for one course: %d\n", base_allocs);

    Course shallow = a; /* struct assignment: copies pointers only */
    printf("shallow shares memory: %d\n", shares_memory(&a, &shallow));
    shallow.students[0].scores[0] = 1000;
    describe("a after write via shallow", &a);
    printf("live allocs unchanged by shallow copy: %d\n", live_allocs == base_allocs);

    Course deep = course_clone(&a);
    printf("deep shares memory: %d, allocs=%d\n", shares_memory(&a, &deep), live_allocs);
    deep.students[1].scores[0] = -1;
    deep.title[0] = 'X';
    describe("a", &a);
    describe("deep", &deep);
    check(a.students[1].scores[0] == 60 && a.title[0] == 'S', "deep independent");

    /* the embedded `best` is a by-value member: assigning it aliases its heap pointers */
    Student alias = a.best;
    printf("best alias same name ptr: %d\n", alias.name == a.best.name);

    Course moved = course_move(&a);
    printf("after move: a.title=%s a.students=%s moved ok=%d\n", a.title ? "set" : "null",
           a.students ? "set" : "null", moved.n == 3);
    check(live_allocs == 2 * base_allocs, "move allocates nothing");
    describe("moved", &moved);

    course_free(&a); /* freeing the moved-from course is safe */
    course_free(&moved);
    (void)shallow; /* dangling now: must not be touched */
    course_free(&deep);
    printf("live allocs at end: %d\n", live_allocs);
    check(live_allocs == 0, "leak");
    return 0;
}
