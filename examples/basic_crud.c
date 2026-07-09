/*
 * Example: basic CRUD operations with the MongrelDB C client.
 *
 * Build (from the repo root):
 *
 *   cc -std=c99 -Iinclude examples/basic_crud.c src/mongreldb.c \
 *       $(pkg-config --cflags --libs libcurl) -o examples/basic_crud
 *   ./examples/basic_crud
 *
 * Requires a mongreldb-server daemon running on http://127.0.0.1:8453, or
 * point MONGRELDB_URL at a running daemon.
 *
 * Creates a table, inserts three rows, counts them, queries all rows, upserts
 * (updates) one row by primary key, deletes one row, then drops the table.
 * Progress is printed at every step.
 */

#include <mongreldb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DB_URL_DEFAULT "http://127.0.0.1:8453"
#define TABLE_PREFIX   "example_crud_"
#define TABLE_SIZE     64

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

/* Build a three-cell input row as a C99 compound literal. Cast to
 * (const mongreldb_input_cell *) so it can be passed directly to the count-
 * taking put/upsert helpers. */
#define ROW(id, name, score)                                                    \
    ((const mongreldb_input_cell[]){                                            \
        {1, {MDB_VAL_INT64,  .v.i64 = (id)}},                                   \
        {2, {MDB_VAL_STRING, .v.str = (name)}},                                 \
        {3, {MDB_VAL_DOUBLE, .v.f64 = (score)}},                                \
    })

/* Print every cell of a query result. */
static void print_result(const mongreldb_result *res) {
    for (size_t i = 0; i < res->count; i++) {
        printf("  { ");
        for (size_t j = 0; j < res->rows[i].count; j++) {
            mongreldb_cell c = res->rows[i].cells[j];
            printf("col%lld=", (long long)c.column_id);
            switch (c.value.tag) {
                case MDB_VAL_INT64:  printf("%lld", (long long)c.value.v.i64); break;
                case MDB_VAL_DOUBLE: printf("%g", c.value.v.f64); break;
                case MDB_VAL_STRING: printf("%s", c.value.v.str); break;
                case MDB_VAL_BOOL:   printf("%s", c.value.v.b ? "true" : "false"); break;
                default:             printf("null"); break;
            }
            if (j + 1 < res->rows[i].count) printf(", ");
        }
        printf(" }\n");
    }
}

int main(void) {
    /* Per-run unique suffix (unix time) keeps every invocation isolated on a
     * shared daemon. */
    char table[TABLE_SIZE];
    snprintf(table, sizeof(table), "%s%ld", TABLE_PREFIX, (long)time(NULL));

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

    /* 1. Health check; bail out if the daemon is unreachable. */
    if (mongreldb_health(db) != MDB_OK) {
        fprintf(stderr, "daemon not reachable at %s: %s\n", url, mongreldb_last_error(db));
        mongreldb_close(db);
        return 1;
    }
    printf("Connected to MongrelDB\n");

    /* 2. Create the table. */
    int64_t tid = 0;
    if (mongreldb_create_table(db, table, kCols, 3, &tid) != MDB_OK) {
        fprintf(stderr, "create_table failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    table_created = 1;
    printf("Created table %s (id %lld)\n", table, (long long)tid);

    /* 3. Insert three rows. */
    if (mongreldb_put(db, table, ROW(1, "Alice", 95.5), 3, NULL) != MDB_OK) {
        fprintf(stderr, "put failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    if (mongreldb_put(db, table, ROW(2, "Bob", 82.0), 3, NULL) != MDB_OK) {
        fprintf(stderr, "put failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    if (mongreldb_put(db, table, ROW(3, "Carol", 78.3), 3, NULL) != MDB_OK) {
        fprintf(stderr, "put failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Inserted 3 rows\n");

    /* 4. Count. */
    int64_t n = 0;
    if (mongreldb_count(db, table, &n) != MDB_OK) {
        fprintf(stderr, "count failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Total rows: %lld\n", (long long)n);

    /* 5. Query all rows (no conditions, no projection, no limit). */
    mongreldb_result res;
    if (mongreldb_query(db, table, NULL, 0, NULL, 0, 0, &res) != MDB_OK) {
        fprintf(stderr, "query failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Query returned %zu rows:\n", res.count);
    print_result(&res);

    /* 6. Upsert (update) Alice's score. update_cells supplies the values
     *    written on a primary-key conflict. */
    mongreldb_input_cell up[] = {
        {1, {MDB_VAL_INT64,  .v.i64 = 1}},
        {2, {MDB_VAL_STRING, .v.str = "Alice"}},
        {3, {MDB_VAL_DOUBLE, .v.f64 = 100.0}},
    };
    mongreldb_input_cell upd[] = {
        {2, {MDB_VAL_STRING, .v.str = "Alice"}},
        {3, {MDB_VAL_DOUBLE, .v.f64 = 100.0}},
    };
    if (mongreldb_upsert(db, table, up, 3, upd, 2, NULL) != MDB_OK) {
        fprintf(stderr, "upsert failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Upserted Alice's score to 100.0\n");
    if (mongreldb_count(db, table, &n) != MDB_OK) {
        fprintf(stderr, "count failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Total rows after upsert: %lld\n", (long long)n);

    /* 7. Delete Carol (primary key 3). */
    mongreldb_value pk = {MDB_VAL_INT64, .v.i64 = 3};
    if (mongreldb_delete_by_pk(db, table, &pk) != MDB_OK) {
        fprintf(stderr, "delete_by_pk failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    if (mongreldb_count(db, table, &n) != MDB_OK) {
        fprintf(stderr, "count failed: %s\n", mongreldb_last_error(db));
        goto cleanup;
    }
    printf("Deleted Carol; remaining rows: %lld\n", (long long)n);

cleanup:
    /* Guaranteed cleanup: drop the table if it was created, then close. */
    if (table_created) {
        if (mongreldb_drop_table(db, table) == MDB_OK) {
            printf("Dropped table %s\n", table);
        } else {
            fprintf(stderr, "drop_table failed: %s\n", mongreldb_last_error(db));
        }
    }
    mongreldb_close(db);
    return 0;
}
