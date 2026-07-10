/*
 * Example: atomic batch transactions with an idempotent retry in C.
 *
 * Build (from the repo root):
 *
 *   cc -std=c99 -Iinclude examples/transactions.c src/mongreldb.c \
 *       $(pkg-config --cflags --libs libcurl) -o examples/transactions
 *   ./examples/transactions
 *
 * Requires a mongreldb-server daemon running on http://127.0.0.1:8453, or
 * point MONGRELDB_URL at a running daemon.
 *
 * Creates a table, opens one transaction, stages three puts, and commits
 * them atomically. It then verifies the row count. Finally it stages a
 * fourth put and commits it twice with the SAME idempotency key: the
 * daemon replays the first commit's result so the second commit is a
 * no-op. The table is dropped at the end (even on error).
 *
 * The "status" column is an enum ("active" | "inactive" | "paused") with a
 * default of "active"; the "score" column has a numeric default of "0.0".
 * These are emitted as "enum_variants" and "default_value" keys in the
 * /kit/create_table wire JSON, and the enum constraint is enforced at
 * commit time.
 */

#include <mongreldb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DB_URL_DEFAULT "http://127.0.0.1:8453"
#define TABLE_PREFIX   "example_txn_"
#define TABLE_SIZE     64

/* Column schema shared across all examples:
 *   col 1 = id (int64, primary key)
 *   col 2 = name (varchar)
 *   col 3 = score (float64, default "0.0")
 *   col 4 = status (varchar, enum ["active","inactive","paused"], default "active")
 */
static const char *const kStatusVariants[] = {"active", "inactive", "paused"};
static const mongreldb_column kCols[] = {
    {1, "id",     "int64",   /*primary_key=*/1, /*nullable=*/0,
        /*enum_variants=*/NULL,        /*enum_variants_len=*/0,
        /*default_value=*/NULL},
    {2, "name",   "varchar", /*primary_key=*/0, /*nullable=*/0,
        /*enum_variants=*/NULL,        /*enum_variants_len=*/0,
        /*default_value=*/NULL},
    {3, "score",  "float64", /*primary_key=*/0, /*nullable=*/0,
        /*enum_variants=*/NULL,        /*enum_variants_len=*/0,
        /*default_value=*/"0.0"},
    {4, "status", "varchar", /*primary_key=*/0, /*nullable=*/0,
        /*enum_variants=*/kStatusVariants, /*enum_variants_len=*/3,
        /*default_value=*/"active"},
};

/* Build a four-cell input row as a C99 compound literal. */
#define ROW(id, name, score, status)                                            \
    ((const mongreldb_input_cell[]){                                            \
        {1, {MDB_VAL_INT64,  .v.i64 = (id)}},                                   \
        {2, {MDB_VAL_STRING, .v.str = (name)}},                                 \
        {3, {MDB_VAL_DOUBLE, .v.f64 = (score)}},                                \
        {4, {MDB_VAL_STRING, .v.str = (status)}},                               \
    })

/* Build a PUT op referencing the row array. Uses designated initializers so
 * the mongreldb_op aggregate is initialized cleanly under -Wall -Wextra: the
 * omitted members (update_cells, row_id, pk_value) are zero-initialized, which
 * avoids the "braces around scalar initializer" a positional initializer for
 * the pk_value union member would trigger. The macro argument is named row_cs
 * (not `cells`) to avoid clashing with the struct member designator `.cells`. */
#define PUT_OP(row_cs)                                                          \
    ((mongreldb_op){                                                            \
        .type = MDB_OP_PUT,                                                     \
        .table = g_table,                                                       \
        .cells = (row_cs),                                                      \
        .cell_count = 4,                                                        \
    })

/* The per-run table name: filled in by main() and referenced by the PUT_OP
 * macro. Idempotency keys are built locally in main(). */
static char g_table[TABLE_SIZE];

int main(void) {
    /* Per-run unique suffix (unix time) keeps every invocation isolated on a
     * shared daemon, and the idempotency key must be unique per run too: a
     * reused key replays the original result and silently drops the new batch. */
    long ts = (long)time(NULL);
    snprintf(g_table, sizeof(g_table), "%s%ld", TABLE_PREFIX, ts);
    char txn_key[TABLE_SIZE + 8];
    snprintf(txn_key, sizeof(txn_key), "example-txn-key-%ld", ts);

    const char *url = getenv("MONGRELDB_URL");
    if (url == NULL || url[0] == '\0') {
        url = DB_URL_DEFAULT;
    }

    mongreldb_client *db = mongreldb_connect(url);
    if (!db) {
        fprintf(stderr, "mongreldb_connect failed\n");
        return 1;
    }

    int table_created = 0;

    /* Status starts in the failure state and is cleared only after all steps
     * complete. This way an early `goto cleanup` returns non-zero, so CI can
     * detect that the example failed. */
    int status = 1;

    /* 1. Health check; bail out if the daemon is unreachable. */
    if (mongreldb_health(db) != MDB_OK) {
        fprintf(stderr, "daemon not reachable at %s: %s\n", url, mongreldb_last_error(db));
        mongreldb_close(db);
        return 1;
    }
    printf("Connected to MongrelDB\n");

    /* 2. Create the table. */
    int64_t tid = 0;
    if (mongreldb_create_table(db, g_table, kCols, 4, &tid) != MDB_OK) {
        fprintf(stderr, "create_table failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    table_created = 1;
    printf("Created table %s (id %lld)\n", g_table, (long long)tid);

    /* 3. Stage three puts and commit them atomically. Each status is one of
     *    the allowed enum variants - the server enforces this at commit. */
    mongreldb_op batch1[] = {
        PUT_OP(ROW(1, "Alice", 95.5, "active")),
        PUT_OP(ROW(2, "Bob",   82.0, "inactive")),
        PUT_OP(ROW(3, "Carol", 78.3, "paused")),
    };
    if (mongreldb_commit(db, batch1, 3, NULL) != MDB_OK) {
        fprintf(stderr, "commit (3 puts) failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Committed transaction with 3 puts\n");

    /* 4. Verify the row count. */
    int64_t n = 0;
    if (mongreldb_count(db, g_table, &n) != MDB_OK) {
        fprintf(stderr, "count failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Total rows after commit: %lld\n", (long long)n);

    /* 5. Idempotent retry: stage a fourth put and commit twice with the
     *    same idempotency key. The second commit is replayed as a no-op. */
    mongreldb_op batch2[] = {
        PUT_OP(ROW(4, "Dave", 60.0, "active")),
    };
    if (mongreldb_commit(db, batch2, 1, txn_key) != MDB_OK) {
        fprintf(stderr, "commit (4th put, first attempt) failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Committed 4th put with idempotency key %s\n", txn_key);

    if (mongreldb_commit(db, batch2, 1, txn_key) != MDB_OK) {
        fprintf(stderr, "commit (4th put, idempotent retry) failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Recommitted with same key (idempotent replay)\n");

    if (mongreldb_count(db, g_table, &n) != MDB_OK) {
        fprintf(stderr, "count failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Total rows after idempotent retry: %lld\n", (long long)n);

    /* All steps completed. */
    status = 0;

cleanup:
    /* Guaranteed cleanup: drop the table if it was created, then close. */
    if (table_created) {
        if (mongreldb_drop_table(db, g_table) == MDB_OK) {
            printf("Dropped table %s\n", g_table);
        } else {
            fprintf(stderr, "drop_table failed: %s\n", mongreldb_last_error(db));
        }
    }
    mongreldb_close(db);
    return status;
}
