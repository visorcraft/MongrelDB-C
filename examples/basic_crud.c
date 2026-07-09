/*
 * Example: basic CRUD operations with the MongrelDB C client.
 *
 * Build (from the repo root):
 *
 *   cc -std=c99 -Iinclude examples/basic_crud.c src/mongreldb.c \
 *       $(pkg-config --cflags --libs libcurl) -o examples/basic_crud
 *   ./examples/basic_crud
 *
 * Requires a mongreldb-server daemon running on http://127.0.0.1:8453.
 *
 * Creates a table, inserts three rows, counts them, queries all rows, upserts
 * (updates) one row by primary key, deletes one row, then drops the table.
 * Progress is printed at every step.
 */

#include <mongreldb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_URL "http://127.0.0.1:8453"
#define TABLE  "example_crud"

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

    /* 3. Insert three rows. */
    if (mongreldb_put(db, TABLE, ROW(1, "Alice", 95.5), 3, NULL) != MDB_OK) {
        die(db, "put");
    }
    if (mongreldb_put(db, TABLE, ROW(2, "Bob", 82.0), 3, NULL) != MDB_OK) {
        die(db, "put");
    }
    if (mongreldb_put(db, TABLE, ROW(3, "Carol", 78.3), 3, NULL) != MDB_OK) {
        die(db, "put");
    }
    printf("Inserted 3 rows\n");

    /* 4. Count. */
    int64_t n = 0;
    if (mongreldb_count(db, TABLE, &n) != MDB_OK) {
        die(db, "count");
    }
    printf("Total rows: %lld\n", (long long)n);

    /* 5. Query all rows (no conditions, no projection, no limit). */
    mongreldb_result res;
    if (mongreldb_query(db, TABLE, NULL, 0, NULL, 0, 0, &res) != MDB_OK) {
        die(db, "query");
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
    if (mongreldb_upsert(db, TABLE, up, 3, upd, 2, NULL) != MDB_OK) {
        die(db, "upsert");
    }
    printf("Upserted Alice's score to 100.0\n");
    if (mongreldb_count(db, TABLE, &n) != MDB_OK) {
        die(db, "count");
    }
    printf("Total rows after upsert: %lld\n", (long long)n);

    /* 7. Delete Carol (primary key 3). */
    mongreldb_value pk = {MDB_VAL_INT64, .v.i64 = 3};
    if (mongreldb_delete_by_pk(db, TABLE, &pk) != MDB_OK) {
        die(db, "delete_by_pk");
    }
    if (mongreldb_count(db, TABLE, &n) != MDB_OK) {
        die(db, "count");
    }
    printf("Deleted Carol; remaining rows: %lld\n", (long long)n);

    /* 8. Cleanup. */
    mongreldb_drop_table(db, TABLE);
    printf("Dropped table %s\n", TABLE);

    mongreldb_close(db);
    return 0;
}
