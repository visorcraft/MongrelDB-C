/* test_mongreldb.c - live integration tests for the MongrelDB C client.
 *
 * These boot a real mongreldb-server daemon and exercise the full client
 * surface against it. They skip automatically when no daemon binary is
 * available.
 *
 * Binary resolution order:
 *   1. MONGRELDB_SERVER env var (path to the server binary).
 *   2. ./bin/mongreldb-server (downloaded by CI or `make server`).
 *   3. mongreldb-server on PATH.
 *
 * Or point at an already-running daemon with MONGRELDB_URL.
 *
 * Licensing: MIT OR Apache-2.0.
 */

/* Enable POSIX APIs (nanosleep, mkdtemp, kill, fork, etc.) under strict C99. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _BSD_SOURCE

#include "mongreldb.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/* ── Tiny test framework ───────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;
static const char *g_current = "";

#define TEST(name)                                                             \
    static void name(void);                                                    \
    static void name(void)

#define RUN(name)                                                              \
    do {                                                                       \
        g_current = #name;                                                     \
        printf("== %s\n", #name);                                              \
        int before = g_fail;                                                   \
        name();                                                                \
        if (g_fail == before) {                                                \
            g_pass++;                                                          \
        }                                                                      \
    } while (0)

static void fail_impl(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    printf("  FAIL %s:%d: ", file, line);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    g_fail++;
}

static void skip_impl(const char *reason) {
    printf("  SKIP: %s\n", reason ? reason : "(no daemon)");
    g_skip++;
}

#define FAIL(...) fail_impl(__FILE__, __LINE__, __VA_ARGS__)
#define SKIP(reason)                                                           \
    do {                                                                       \
        skip_impl(reason);                                                     \
        return;                                                                \
    } while (0)

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            FAIL(__VA_ARGS__);                                                 \
            return;                                                            \
        }                                                                      \
    } while (0)

#define CHECK_RC(rc)                                                           \
    do {                                                                       \
        if ((rc) != MDB_OK) {                                                  \
            FAIL("expected MDB_OK, got %d: %s", (rc),                          \
                 mongreldb_last_error(g_client));                              \
            return;                                                            \
        }                                                                      \
    } while (0)

/* ── Daemon harness ────────────────────────────────────────────────────── */

static mongreldb_client *g_client = NULL;
static char g_url[128];
static pid_t g_server_pid = 0;

static int is_executable(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return (st.st_mode & 0111) != 0;
}

static const char *resolve_server_binary(void) {
    const char *env = getenv("MONGRELDB_SERVER");
    if (env && *env && is_executable(env)) {
        return env;
    }
    if (is_executable("./bin/mongreldb-server")) {
        return "./bin/mongreldb-server";
    }
    /* PATH lookup. */
    char path[4096];
    const char *pp = getenv("PATH");
    if (pp) {
        const char *p = pp;
        while (*p) {
            const char *colon = strchr(p, ':');
            size_t len = colon ? (size_t)(colon - p) : strlen(p);
            if (len > 0 && len + 32 < sizeof(path)) {
                memcpy(path, p, len);
                path[len] = '/';
                memcpy(path + len + 1, "mongreldb-server", 16);
                path[len + 17] = '\0';
                if (is_executable(path)) {
                    /* Stash in a static buffer; fine for a test main. */
                    static char found[4096];
                    memcpy(found, path, len + 17);
                    found[len + 17] = '\0';
                    return found;
                }
            }
            if (!colon) {
                break;
            }
            p = colon + 1;
        }
    }
    return NULL;
}

static int free_port(void) {
#ifdef __linux__
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return 0;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);
    addr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return 0;
    }
    socklen_t alen = sizeof(addr);
    int port = 0;
    if (getsockname(s, (struct sockaddr *)&addr, &alen) == 0) {
        port = ntohs(addr.sin_port);
    }
    close(s);
    return port;
#else
    return 8453;
#endif
}

static int daemon_healthy(const char *url) {
    mongreldb_client *c = mongreldb_connect(url);
    if (!c) {
        return 0;
    }
    int ok = (mongreldb_health(c) == MDB_OK);
    mongreldb_close(c);
    return ok;
}

static int wait_for_health(const char *url, int max_seconds) {
    for (int i = 0; i < max_seconds * 2; i++) {
        if (daemon_healthy(url)) {
            return 1;
        }
        struct timespec ts = {0, 500 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void setup_daemon(void) {
    const char *existing = getenv("MONGRELDB_URL");
    if (existing && *existing) {
        if (daemon_healthy(existing)) {
            strncpy(g_url, existing, sizeof(g_url) - 1);
            g_client = mongreldb_connect(g_url);
            return;
        }
        fprintf(stderr, "mongreldb: MONGRELDB_URL=%s not reachable\n", existing);
        exit(1);
    }

    const char *bin = resolve_server_binary();
    if (!bin) {
        fprintf(stderr, "--- no mongreldb-server binary; live tests skipped\n");
        return;
    }

    int port = free_port();
    if (port == 0) {
        fprintf(stderr, "mongreldb: no free port\n");
        return;
    }
    snprintf(g_url, sizeof(g_url), "http://127.0.0.1:%d", port);

    char tmpl[] = "/tmp/mongreldb-c-test-XXXXXX";
    char *data_dir = mkdtemp(tmpl);
    if (!data_dir) {
        fprintf(stderr, "mongreldb: mkdtemp failed: %s\n", strerror(errno));
        return;
    }

    char port_arg[16];
    snprintf(port_arg, sizeof(port_arg), "%d", port);

    g_server_pid = fork();
    if (g_server_pid < 0) {
        fprintf(stderr, "mongreldb: fork failed: %s\n", strerror(errno));
        return;
    }
    if (g_server_pid == 0) {
        /* Child: exec the daemon. Redirect stdio to /dev/null. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) {
                close(devnull);
            }
        }
        execl(bin, bin, data_dir, "--port", port_arg, (char *)NULL);
        /* If exec fails, exit so the parent notices. */
        _exit(127);
    }

    if (!wait_for_health(g_url, 40)) {
        fprintf(stderr, "mongreldb: daemon did not become healthy at %s\n", g_url);
        kill(g_server_pid, SIGKILL);
        waitpid(g_server_pid, NULL, 0);
        g_server_pid = 0;
        return;
    }
    g_client = mongreldb_connect(g_url);
}

static void teardown_daemon(void) {
    if (g_client) {
        mongreldb_close(g_client);
        g_client = NULL;
    }
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGTERM);
        int status;
        /* Give it a moment, then force. */
        for (int i = 0; i < 20; i++) {
            if (waitpid(g_server_pid, &status, WNOHANG) == g_server_pid) {
                g_server_pid = 0;
                return;
            }
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        kill(g_server_pid, SIGKILL);
        waitpid(g_server_pid, NULL, 0);
        g_server_pid = 0;
    }
}

#define SKIP_IF_NO_DAEMON()                                                    \
    do {                                                                       \
        if (!g_client) {                                                       \
            SKIP("no mongreldb-server available");                             \
        }                                                                      \
    } while (0)

/* ── Helpers ───────────────────────────────────────────────────────────── */

static mongreldb_column int_col(int64_t id, const char *name, int pk) {
    mongreldb_column c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.name = name;
    c.ty = "int64";
    c.primary_key = pk;
    c.nullable = !pk;
    return c;
}

static mongreldb_column float_col(int64_t id, const char *name) {
    mongreldb_column c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.name = name;
    c.ty = "float64";
    c.primary_key = 0;
    c.nullable = 0;
    return c;
}

static mongreldb_input_cell icell_i64(int64_t col, int64_t v) {
    mongreldb_input_cell c;
    memset(&c, 0, sizeof(c));
    c.column_id = col;
    c.value.tag = MDB_VAL_INT64;
    c.value.v.i64 = v;
    return c;
}

static mongreldb_input_cell icell_f64(int64_t col, double v) {
    mongreldb_input_cell c;
    memset(&c, 0, sizeof(c));
    c.column_id = col;
    c.value.tag = MDB_VAL_DOUBLE;
    c.value.v.f64 = v;
    return c;
}

static mongreldb_input_cell icell_str(int64_t col, const char *v) {
    mongreldb_input_cell c;
    memset(&c, 0, sizeof(c));
    c.column_id = col;
    c.value.tag = MDB_VAL_STRING;
    c.value.v.str = v;
    return c;
}

static void fresh_table(const char *name, const mongreldb_column *cols, size_t n) {
    mongreldb_drop_table(g_client, name); /* ignore not-found */
    int64_t tid = 0;
    int rc = mongreldb_create_table(g_client, name, cols, n, &tid);
    if (rc != MDB_OK) {
        FAIL("create_table %s: %s", name, mongreldb_last_error(g_client));
    }
}

/* cell_i64 returns the int64 value for col_id in a result row's flat cells
 * array, or 0 if absent. *found is set to 0/1. */
static int64_t cell_i64(mongreldb_row row, int64_t col_id, int *found) {
    if (found) {
        *found = 0;
    }
    for (size_t i = 0; i < row.count; i++) {
        if (row.cells[i].column_id == col_id &&
            row.cells[i].value.tag == MDB_VAL_INT64) {
            if (found) {
                *found = 1;
            }
            return row.cells[i].value.v.i64;
        }
    }
    return 0;
}

/* cell_f64 returns the double value for col_id in a result row's flat cells
 * array, or 0.0 if absent. *found is set to 0/1. */
static double cell_f64(mongreldb_row row, int64_t col_id, int *found) {
    if (found) {
        *found = 0;
    }
    for (size_t i = 0; i < row.count; i++) {
        if (row.cells[i].column_id == col_id &&
            row.cells[i].value.tag == MDB_VAL_DOUBLE) {
            if (found) {
                *found = 1;
            }
            return row.cells[i].value.v.f64;
        }
    }
    return 0.0;
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

TEST(test_health) {
    SKIP_IF_NO_DAEMON();
    int rc = mongreldb_health(g_client);
    CHECK(rc == MDB_OK, "health failed: %s", mongreldb_last_error(g_client));
}

TEST(test_create_table_and_count) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1), float_col(2, "amount") };
    fresh_table("c_tbl_count", cols, 2);
    int64_t n = -1;
    int rc = mongreldb_count(g_client, "c_tbl_count", &n);
    CHECK_RC(rc);
    CHECK(n == 0, "expected 0 rows, got %lld", (long long)n);
}

TEST(test_put_and_count) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1), float_col(2, "amount") };
    fresh_table("c_put", cols, 2);

    mongreldb_input_cell r1[] = { icell_i64(1, 1), icell_f64(2, 99.5) };
    mongreldb_input_cell r2[] = { icell_i64(1, 2), icell_f64(2, 150.0) };
    CHECK_RC(mongreldb_put(g_client, "c_put", r1, 2, NULL));
    CHECK_RC(mongreldb_put(g_client, "c_put", r2, 2, NULL));

    int64_t n = -1;
    CHECK_RC(mongreldb_count(g_client, "c_put", &n));
    CHECK(n == 2, "expected 2 rows, got %lld", (long long)n);
}

TEST(test_upsert) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1), float_col(2, "amount") };
    fresh_table("c_upsert", cols, 2);

    mongreldb_input_cell r1[] = { icell_i64(1, 1), icell_f64(2, 10.0) };
    CHECK_RC(mongreldb_put(g_client, "c_upsert", r1, 2, NULL));

    /* Upsert the same PK with an update on conflict. */
    mongreldb_input_cell up[] = { icell_i64(1, 1), icell_f64(2, 20.0) };
    mongreldb_input_cell upd[] = { icell_f64(2, 20.0) };
    CHECK_RC(mongreldb_upsert(g_client, "c_upsert", up, 2, upd, 1, NULL));

    int64_t n = -1;
    CHECK_RC(mongreldb_count(g_client, "c_upsert", &n));
    CHECK(n == 1, "expected 1 row after upsert, got %lld", (long long)n);

    /* Query the row back and verify the updated value landed. */
    mongreldb_condition cond;
    memset(&cond, 0, sizeof(cond));
    cond.kind = MDB_COND_PK;
    cond.int_value = 1;
    cond.int_set = 1;

    mongreldb_result res;
    int rc = mongreldb_query(g_client, "c_upsert", &cond, 1, NULL, 0, 0, &res);
    CHECK_RC(rc);
    CHECK(res.count == 1, "expected 1 row from pk query, got %zu", res.count);
    int found = 0;
    int64_t pk = cell_i64(res.rows[0], 1, &found);
    CHECK(found && pk == 1, "expected returned pk 1, got %lld", (long long)pk);
    double amt = cell_f64(res.rows[0], 2, &found);
    CHECK(found && amt == 20.0, "expected updated amount 20.0, got %g", amt);
}

TEST(test_query_by_pk) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1) };
    fresh_table("c_pk", cols, 1);

    mongreldb_input_cell r1[] = { icell_i64(1, 42) };
    mongreldb_input_cell r2[] = { icell_i64(1, 43) };
    CHECK_RC(mongreldb_put(g_client, "c_pk", r1, 1, NULL));
    CHECK_RC(mongreldb_put(g_client, "c_pk", r2, 1, NULL));

    mongreldb_condition cond;
    memset(&cond, 0, sizeof(cond));
    cond.kind = MDB_COND_PK;
    cond.int_value = 42;
    cond.int_set = 1;

    mongreldb_result res;
    int rc = mongreldb_query(g_client, "c_pk", &cond, 1, NULL, 0, 0, &res);
    CHECK_RC(rc);
    CHECK(res.count == 1, "expected 1 row, got %zu", res.count);
    /* The returned row must carry the queried PK value. */
    int found = 0;
    int64_t pk = cell_i64(res.rows[0], 1, &found);
    CHECK(found && pk == 42, "expected returned pk 42, got %lld", (long long)pk);
}

TEST(test_query_range) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1), int_col(2, "amount", 0) };
    fresh_table("c_range", cols, 2);

    mongreldb_input_cell r1[] = { icell_i64(1, 1), icell_i64(2, 50) };
    mongreldb_input_cell r2[] = { icell_i64(1, 2), icell_i64(2, 120) };
    mongreldb_input_cell r3[] = { icell_i64(1, 3), icell_i64(2, 200) };
    CHECK_RC(mongreldb_put(g_client, "c_range", r1, 2, NULL));
    CHECK_RC(mongreldb_put(g_client, "c_range", r2, 2, NULL));
    CHECK_RC(mongreldb_put(g_client, "c_range", r3, 2, NULL));

    mongreldb_condition cond;
    memset(&cond, 0, sizeof(cond));
    cond.kind = MDB_COND_RANGE;
    cond.column_id = 2;
    cond.lo = 100;
    cond.lo_set = 1;
    cond.hi = 150;
    cond.hi_set = 1;

    mongreldb_result res;
    int rc = mongreldb_query(g_client, "c_range", &cond, 1, NULL, 0, 0, &res);
    CHECK_RC(rc);
    /* Only the row with amount=120 (pk=2) falls in [100, 150]. */
    CHECK(res.count == 1, "expected exactly 1 matching row, got %zu", res.count);
    CHECK(res.truncated == 0, "result should not be truncated");
    int found = 0;
    int64_t pk = cell_i64(res.rows[0], 1, &found);
    CHECK(found && pk == 2, "expected returned pk 2, got %lld", (long long)pk);
    int64_t amt = cell_i64(res.rows[0], 2, &found);
    CHECK(found && amt == 120, "expected returned amount 120, got %lld", (long long)amt);
}

TEST(test_transaction_commit) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1) };
    fresh_table("c_txn", cols, 1);

    mongreldb_op ops[3];
    memset(ops, 0, sizeof(ops));
    ops[0].type = MDB_OP_PUT;
    ops[0].table = "c_txn";
    mongreldb_input_cell c0[] = { icell_i64(1, 1) };
    ops[0].cells = c0;
    ops[0].cell_count = 1;
    ops[1].type = MDB_OP_PUT;
    ops[1].table = "c_txn";
    mongreldb_input_cell c1[] = { icell_i64(1, 2) };
    ops[1].cells = c1;
    ops[1].cell_count = 1;
    ops[2].type = MDB_OP_PUT;
    ops[2].table = "c_txn";
    mongreldb_input_cell c2[] = { icell_i64(1, 3) };
    ops[2].cells = c2;
    ops[2].cell_count = 1;

    CHECK_RC(mongreldb_commit(g_client, ops, 3, NULL));
    int64_t n = -1;
    CHECK_RC(mongreldb_count(g_client, "c_txn", &n));
    CHECK(n == 3, "expected 3 rows after commit, got %lld", (long long)n);
}

TEST(test_delete_by_pk) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1) };
    fresh_table("c_del", cols, 1);

    mongreldb_input_cell r[] = { icell_i64(1, 5) };
    CHECK_RC(mongreldb_put(g_client, "c_del", r, 1, NULL));
    int64_t n = -1;
    CHECK_RC(mongreldb_count(g_client, "c_del", &n));
    CHECK(n == 1, "expected 1 row, got %lld", (long long)n);

    mongreldb_value pk;
    memset(&pk, 0, sizeof(pk));
    pk.tag = MDB_VAL_INT64;
    pk.v.i64 = 5;
    CHECK_RC(mongreldb_delete_by_pk(g_client, "c_del", &pk));
    CHECK_RC(mongreldb_count(g_client, "c_del", &n));
    CHECK(n == 0, "expected 0 rows after delete, got %lld", (long long)n);
}

TEST(test_sql) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1), int_col(2, "amount", 0) };
    fresh_table("c_sql", cols, 2);

    int64_t n = -1;
    CHECK_RC(mongreldb_count(g_client, "c_sql", &n));
    CHECK(n == 0, "expected 0 rows before SQL INSERT, got %lld", (long long)n);

    /* INSERT via SQL must increase the row count. */
    const char *body = NULL;
    int rc = mongreldb_sql(g_client,
                           "INSERT INTO c_sql (id, amount) VALUES (10, 42)",
                           &body);
    CHECK(rc == MDB_OK, "SQL INSERT failed: %s", mongreldb_last_error(g_client));
    CHECK_RC(mongreldb_count(g_client, "c_sql", &n));
    CHECK(n == 1, "expected count to increase to 1 after INSERT, got %lld",
          (long long)n);

    /* JSON SQL mode must return the inserted row (a non-empty JSON array). */
    rc = mongreldb_sql(g_client, "SELECT id, amount FROM c_sql", &body);
    CHECK(rc == MDB_OK, "SQL SELECT failed: %s", mongreldb_last_error(g_client));
    CHECK(body != NULL && body[0] == '[', "expected JSON array body for SELECT");
}

TEST(test_string_values) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[3];
    cols[0] = int_col(1, "id", 1);
    cols[1].id = 2; cols[1].name = "label"; cols[1].ty = "varchar";
    cols[1].primary_key = 0; cols[1].nullable = 0;
    cols[2] = float_col(3, "amount");
    fresh_table("c_str", cols, 3);

    mongreldb_input_cell r[] = {
        icell_i64(1, 1),
        icell_str(2, "hello world"),
        icell_f64(3, 1.5),
    };
    CHECK_RC(mongreldb_put(g_client, "c_str", r, 3, NULL));

    /* Round-trip via PK query and read the string cell back. */
    mongreldb_condition cond;
    memset(&cond, 0, sizeof(cond));
    cond.kind = MDB_COND_PK;
    cond.int_value = 1;
    cond.int_set = 1;

    mongreldb_result res;
    int rc = mongreldb_query(g_client, "c_str", &cond, 1, NULL, 0, 0, &res);
    CHECK_RC(rc);
    CHECK(res.count == 1, "expected 1 row, got %zu", res.count);

    const char *label = NULL;
    for (size_t i = 0; i < res.rows[0].count; i++) {
        if (res.rows[0].cells[i].column_id == 2 &&
            res.rows[0].cells[i].value.tag == MDB_VAL_STRING) {
            label = res.rows[0].cells[i].value.v.str;
        }
    }
    CHECK(label != NULL && strcmp(label, "hello world") == 0,
          "expected label 'hello world', got %s", label ? label : "(null)");
}

TEST(test_table_names) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1) };
    fresh_table("c_tables", cols, 1);

    const char **names = NULL;
    size_t n = 0;
    int rc = mongreldb_table_names(g_client, &names, &n);
    CHECK_RC(rc);
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], "c_tables") == 0) {
            found = 1;
            break;
        }
    }
    CHECK(found, "table list missing c_tables");
}

TEST(test_schema_for) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1), float_col(2, "amount") };
    fresh_table("c_schema", cols, 2);

    const char *body = NULL;
    int rc = mongreldb_schema_for(g_client, "c_schema", &body);
    CHECK(rc == MDB_OK, "schema_for failed: %s", mongreldb_last_error(g_client));
    CHECK(body != NULL && body[0] != '\0', "expected non-empty schema body");
}

TEST(test_error_not_found) {
    SKIP_IF_NO_DAEMON();
    const char *body = NULL;
    int rc = mongreldb_schema_for(g_client, "c_does_not_exist_xyz", &body);
    CHECK(rc == MDB_ERR_NOT_FOUND,
          "expected MDB_ERR_NOT_FOUND (%d), got %d (%s)",
          MDB_ERR_NOT_FOUND, rc, mongreldb_last_error(g_client));
}

TEST(test_idempotency_key) {
    SKIP_IF_NO_DAEMON();
    mongreldb_column cols[] = { int_col(1, "id", 1) };
    fresh_table("c_idem", cols, 1);

    mongreldb_input_cell r[] = { icell_i64(1, 1) };
    /* First commit with the key succeeds. */
    CHECK_RC(mongreldb_put(g_client, "c_idem", r, 1, "idem-key-1"));
    int64_t n = -1;
    CHECK_RC(mongreldb_count(g_client, "c_idem", &n));
    CHECK(n == 1, "expected 1 row, got %lld", (long long)n);

    /* A second put with a DIFFERENT value but the SAME key replays the
     * original result; the row count stays at 1. */
    mongreldb_input_cell r2[] = { icell_i64(1, 2) };
    mongreldb_put(g_client, "c_idem", r2, 1, "idem-key-1");
    CHECK_RC(mongreldb_count(g_client, "c_idem", &n));
    CHECK(n == 1, "expected 1 row after duplicate idempotent commit, got %lld",
          (long long)n);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    setup_daemon();

    RUN(test_health);
    RUN(test_create_table_and_count);
    RUN(test_put_and_count);
    RUN(test_upsert);
    RUN(test_query_by_pk);
    RUN(test_query_range);
    RUN(test_transaction_commit);
    RUN(test_delete_by_pk);
    RUN(test_string_values);
    RUN(test_sql);
    RUN(test_table_names);
    RUN(test_schema_for);
    RUN(test_error_not_found);
    RUN(test_idempotency_key);

    teardown_daemon();

    printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail > 0 ? 1 : 0;
}
