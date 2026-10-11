/* bench_libmysql.c -- the libmysqlclient baseline for bench.iron.
 *
 * The same workloads with the C API's prepared statements: point SELECT by
 * primary key (results buffered with mysql_stmt_store_result, as the Iron
 * client buffers every result) and single-row INSERT, on one connection
 * and from N threads with a connection each. Run bench.iron first: it
 * creates and fills the tables.
 *
 *   cc -O2 bench_libmysql.c $(mysql_config --cflags --libs) -lpthread
 *   ./a.out host port user password database ops threads rows
 *   (BENCH_TLS=1 in the environment: TLS, SSL_MODE_REQUIRED)
 */
#include <mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *g_host, *g_user, *g_pass, *g_db;
static unsigned g_port;
static long g_rows;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static MYSQL *connect_db(void) {
    MYSQL *m = mysql_init(NULL);
    unsigned int ssl = getenv("BENCH_TLS") ? SSL_MODE_REQUIRED : SSL_MODE_DISABLED;
    mysql_options(m, MYSQL_OPT_SSL_MODE, &ssl);
    bool get_key = true;
    mysql_options(m, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &get_key);
    if (!mysql_real_connect(m, g_host, g_user, g_pass, g_db, g_port, NULL, 0)) {
        fprintf(stderr, "connect: %s\n", mysql_error(m));
        exit(1);
    }
    return m;
}

static long run_selects(MYSQL *m, long start, long ops) {
    MYSQL_STMT *st = mysql_stmt_init(m);
    const char *sql = "SELECT id, name, value FROM bench_t WHERE id = ?";
    if (mysql_stmt_prepare(st, sql, strlen(sql))) { fprintf(stderr, "%s\n", mysql_stmt_error(st)); exit(1); }
    int id_param = 0;
    MYSQL_BIND param;
    memset(&param, 0, sizeof(param));
    param.buffer_type = MYSQL_TYPE_LONG;
    param.buffer = &id_param;
    int out_id = 0, out_value = 0;
    char out_name[65];
    unsigned long name_len = 0;
    MYSQL_BIND res[3];
    memset(res, 0, sizeof(res));
    res[0].buffer_type = MYSQL_TYPE_LONG; res[0].buffer = &out_id;
    res[1].buffer_type = MYSQL_TYPE_STRING; res[1].buffer = out_name;
    res[1].buffer_length = sizeof(out_name); res[1].length = &name_len;
    res[2].buffer_type = MYSQL_TYPE_LONG; res[2].buffer = &out_value;
    long found = 0;
    for (long i = 0; i < ops; i++) {
        id_param = (int)(((start + i) * 7919) % g_rows + 1);
        mysql_stmt_bind_param(st, &param);
        if (mysql_stmt_execute(st)) { fprintf(stderr, "%s\n", mysql_stmt_error(st)); exit(1); }
        mysql_stmt_bind_result(st, res);
        mysql_stmt_store_result(st);
        if (mysql_stmt_fetch(st) == 0 && out_value == id_param * 3) found++;
        mysql_stmt_free_result(st);
    }
    mysql_stmt_close(st);
    return found;
}

static long run_inserts(MYSQL *m, long start, long ops) {
    MYSQL_STMT *st = mysql_stmt_init(m);
    const char *sql = "INSERT INTO bench_ins (name, value) VALUES (?, ?)";
    if (mysql_stmt_prepare(st, sql, strlen(sql))) { fprintf(stderr, "%s\n", mysql_stmt_error(st)); exit(1); }
    char name[] = "row-name";
    unsigned long name_len = 8;
    long long value = 0;
    MYSQL_BIND p[2];
    memset(p, 0, sizeof(p));
    p[0].buffer_type = MYSQL_TYPE_STRING; p[0].buffer = name; p[0].length = &name_len;
    p[1].buffer_type = MYSQL_TYPE_LONGLONG; p[1].buffer = &value;
    long done = 0;
    for (long i = 0; i < ops; i++) {
        value = start + i;
        mysql_stmt_bind_param(st, p);
        if (mysql_stmt_execute(st)) { fprintf(stderr, "%s\n", mysql_stmt_error(st)); exit(1); }
        if (mysql_stmt_affected_rows(st) == 1) done++;
    }
    mysql_stmt_close(st);
    return done;
}

typedef struct Job { int task; long ops; int insert; long ok; } Job;
static pthread_barrier_t g_ready;

static void *worker(void *arg) {
    Job *j = (Job *)arg;
    mysql_thread_init();
    MYSQL *m = connect_db();
    pthread_barrier_wait(&g_ready);
    j->ok = j->insert ? run_inserts(m, (long)j->task * 1000000, j->ops)
                      : run_selects(m, (long)j->task * 1000003, j->ops);
    mysql_close(m);
    mysql_thread_end();
    return NULL;
}

static void report(const char *name, long ops, long ok, double t0) {
    double secs = now() - t0;
    printf("%s ops=%ld ok=%ld secs=%.3f qps=%.0f\n", name, ops, ok, secs, ops / secs);
}

int main(int argc, char **argv) {
    if (argc < 9) {
        fprintf(stderr, "usage: %s host port user password database ops threads rows\n", argv[0]);
        return 2;
    }
    g_host = argv[1]; g_port = (unsigned)atoi(argv[2]); g_user = argv[3];
    g_pass = argv[4]; g_db = argv[5];
    long ops = atol(argv[6]);
    int threads = atoi(argv[7]);
    g_rows = atol(argv[8]);
    mysql_library_init(0, NULL, NULL);

    MYSQL *m = connect_db();
    run_selects(m, 0, g_rows);   /* warm up */
    double t0 = now();
    long ok = run_selects(m, 1, ops);
    report("libmysqlclient select single", ops, ok, t0);
    t0 = now();
    ok = run_inserts(m, 0, ops);
    report("libmysqlclient insert single", ops, ok, t0);
    mysql_close(m);

    /* Threads connect before the clock starts, like the pool's connections. */
    if (threads > 64) threads = 64;
    for (int pass = 0; pass < 2; pass++) {
        pthread_t th[64];
        Job jobs[64];
        long per = ops / threads;
        pthread_barrier_init(&g_ready, NULL, (unsigned)threads + 1);
        for (int i = 0; i < threads; i++) {
            jobs[i] = (Job){ i, per, pass == 1, 0 };
            pthread_create(&th[i], NULL, worker, &jobs[i]);
        }
        pthread_barrier_wait(&g_ready);
        double start = now();
        long total = 0;
        for (int i = 0; i < threads; i++) {
            pthread_join(th[i], NULL);
            total += jobs[i].ok;
        }
        pthread_barrier_destroy(&g_ready);
        char name[64];
        snprintf(name, sizeof(name), "libmysqlclient %s threads x%d", pass ? "insert" : "select", threads);
        report(name, per * threads, total, start);
    }
    mysql_library_end();
    return 0;
}
