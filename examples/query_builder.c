/*
 * Example: native query builder (range + primary-key lookups) in C.
 *
 * Build (from the repo root):
 *
 *   cc -std=c99 -Iinclude examples/query_builder.c src/mongreldb.c \
 *       $(pkg-config --cflags --libs libcurl) -o examples/query_builder
 *   ./examples/query_builder
 *
 * Requires a mongreldb-server daemon running on http://127.0.0.1:8453.
 *
 * Creates a table, loads five rows with varying scores, then runs two
 * native queries: a range scan over score in [60, 90], and an exact
 * primary-key lookup for id == 4. Results are printed, then the table is
 * dropped.
 */

#include <mongreldb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_URL "http://127.0.0.1:8453"
#define TABLE  "example_query"

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

static void die(mongreldb_client *c, const char *what) {
    fprintf(stderr, "%s failed: %s\n", what, mongreldb_last_error(c));
    mongreldb_close(c);
    exit(1);
}

/* Print every cell of a query result. */
static void print_result(const char *label, const mongreldb_result *res) {
    printf("  %s: %zu rows\n", label, res->count);
    for (size_t i = 0; i < res->count; i++) {
        printf("    { ");
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

    /* 3. Load five rows with varying scores. */
    if (mongreldb_put(db, TABLE, ROW(1, "Alice", 40.0), 3, NULL) != MDB_OK) die(db, "put");
    if (mongreldb_put(db, TABLE, ROW(2, "Bob",   65.0), 3, NULL) != MDB_OK) die(db, "put");
    if (mongreldb_put(db, TABLE, ROW(3, "Carol", 82.0), 3, NULL) != MDB_OK) die(db, "put");
    if (mongreldb_put(db, TABLE, ROW(4, "Dave",  91.0), 3, NULL) != MDB_OK) die(db, "put");
    if (mongreldb_put(db, TABLE, ROW(5, "Eve",   12.5), 3, NULL) != MDB_OK) die(db, "put");
    printf("Inserted 5 rows\n");

    /* 4. Range query: 60 <= score <= 90 (both inclusive). */
    mongreldb_condition range_cond;
    memset(&range_cond, 0, sizeof(range_cond));
    range_cond.kind = MDB_COND_RANGE;
    range_cond.column_id = 3;          /* score */
    range_cond.lo = 60.0;
    range_cond.hi = 90.0;
    range_cond.lo_set = 1;
    range_cond.hi_set = 1;
    range_cond.lo_inclusive = 1;
    range_cond.hi_inclusive = 1;

    mongreldb_result res;
    if (mongreldb_query(db, TABLE, &range_cond, 1, NULL, 0, 0, &res) != MDB_OK) {
        die(db, "range query");
    }
    print_result("range [60, 90] on score", &res);

    /* 5. Primary-key lookup: id == 4 (Dave). */
    mongreldb_condition pk_cond;
    memset(&pk_cond, 0, sizeof(pk_cond));
    pk_cond.kind = MDB_COND_PK;
    pk_cond.int_value = 4;
    pk_cond.int_set = 1;

    if (mongreldb_query(db, TABLE, &pk_cond, 1, NULL, 0, 0, &res) != MDB_OK) {
        die(db, "pk query");
    }
    print_result("pk == 4", &res);

    /* 6. Cleanup. */
    mongreldb_drop_table(db, TABLE);
    printf("Dropped table %s\n", TABLE);

    mongreldb_close(db);
    return 0;
}
