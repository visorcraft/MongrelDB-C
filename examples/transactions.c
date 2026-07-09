/*
 * Example: atomic batch transactions with an idempotent retry in C.
 *
 * Build (from the repo root):
 *
 *   cc -std=c99 -Iinclude examples/transactions.c src/mongreldb.c \
 *       $(pkg-config --cflags --libs libcurl) -o examples/transactions
 *   ./examples/transactions
 *
 * Requires a mongreldb-server daemon running on http://127.0.0.1:8453.
 *
 * Creates a table, opens one transaction, stages three puts, and commits
 * them atomically. It then verifies the row count. Finally it stages a
 * fourth put and commits it twice with the SAME idempotency key: the
 * daemon replays the first commit's result so the second commit is a
 * no-op. The table is dropped at the end.
 */

#include <mongreldb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_URL "http://127.0.0.1:8453"
#define TABLE  "example_txn"
#define TXN_KEY "example-txn-key"

/* Column schema shared across all examples:
 *   col 1 = id (int64, primary key)
 *   col 2 = name (varchar)
 *   col 3 = score (float64)
 */
static const mongreldb_column kCols[] = {
    {1, "id", "int64", /*primary_key=*/1, /*nullable=*/0},
    {2, "name", "varchar", /*primary_key=*/0, /*nullable=*/0},
    {3, "score", "float64", /*primary_key=*/0, /*nullable=*/0},
};

/* Build a three-cell input row as a C99 compound literal. */
#define ROW(id, name, score)                                                    \
    ((const mongreldb_input_cell[]){                                            \
        {1, {MDB_VAL_INT64,  .v.i64 = (id)}},                                   \
        {2, {MDB_VAL_STRING, .v.str = (name)}},                                 \
        {3, {MDB_VAL_DOUBLE, .v.f64 = (score)}},                                \
    })

/* Build a PUT op referencing the row array. cell_count is always 3 here. */
#define PUT_OP(cells)                                                            \
    ((mongreldb_op){MDB_OP_PUT, TABLE, (cells), 3, NULL, 0, 0, {{0}}})

static void die(mongreldb_client *c, const char *what) {
    fprintf(stderr, "%s failed: %s\n", what, mongreldb_last_error(c));
    mongreldb_close(c);
    exit(1);
}

int main(void) {
    mongreldb_client *db = mongreldb_connect(DB_URL);
    if (!db) {
        fprintf(stderr, "mongreldb_connect failed\n");
        return 1;
    }

    /* 1. Health check; bail out if the daemon is unreachable. */
    if (mongreldb_health(db) != MDB_OK) {
        fprintf(stderr, "daemon not reachable at %s: %s\n", DB_URL, mongreldb_last_error(db));
        mongreldb_close(db);
        return 1;
    }
    printf("Connected to MongrelDB\n");

    /* 2. Create the table. */
    int64_t tid = 0;
    if (mongreldb_create_table(db, TABLE, kCols, 3, &tid) != MDB_OK) {
        die(db, "create_table");
    }
    printf("Created table %s (id %lld)\n", TABLE, (long long)tid);

    /* 3. Stage three puts and commit them atomically. */
    mongreldb_op batch1[] = {
        PUT_OP(ROW(1, "Alice", 95.5)),
        PUT_OP(ROW(2, "Bob",   82.0)),
        PUT_OP(ROW(3, "Carol", 78.3)),
    };
    if (mongreldb_commit(db, batch1, 3, NULL) != MDB_OK) {
        die(db, "commit (3 puts)");
    }
    printf("Committed transaction with 3 puts\n");

    /* 4. Verify the row count. */
    int64_t n = 0;
    if (mongreldb_count(db, TABLE, &n) != MDB_OK) die(db, "count");
    printf("Total rows after commit: %lld\n", (long long)n);

    /* 5. Idempotent retry: stage a fourth put and commit twice with the
     *    same idempotency key. The second commit is replayed as a no-op. */
    mongreldb_op batch2[] = {
        PUT_OP(ROW(4, "Dave", 60.0)),
    };
    if (mongreldb_commit(db, batch2, 1, TXN_KEY) != MDB_OK) {
        die(db, "commit (4th put, first attempt)");
    }
    printf("Committed 4th put with idempotency key %s\n", TXN_KEY);

    if (mongreldb_commit(db, batch2, 1, TXN_KEY) != MDB_OK) {
        die(db, "commit (4th put, idempotent retry)");
    }
    printf("Recommitted with same key (idempotent replay)\n");

    if (mongreldb_count(db, TABLE, &n) != MDB_OK) die(db, "count");
    printf("Total rows after idempotent retry: %lld\n", (long long)n);

    /* 6. Cleanup. */
    mongreldb_drop_table(db, TABLE);
    printf("Dropped table %s\n", TABLE);

    mongreldb_close(db);
    return 0;
}
