/*
 * Example: basic CRUD with the native embedded MongrelDB Kit engine (Tier 1).
 *
 * Unlike the HTTP client (mongreldb.h), this links libmongreldb_kit directly
 * and runs the engine in-process. No daemon needed.
 *
 * Build (Linux/macOS):
 *   cc -std=c99 -Iinclude examples/native_basic_crud.c \
 *     -L/path/to/native/libs -lmongreldb_kit -lmongreldb \
 *     -lpthread -ldl -lm \
 *     -Wl,-rpath,/path/to/native/libs \
 *     -o native_basic_crud
 *
 * Download the prebuilt libraries from
 * https://github.com/visorcraft/MongrelDB/releases
 * (mongreldb-native-* and mongreldb-kit-native-* archives).
 *
 * This example uses the Kit FFI (mongreldb_kit.h): creates a database
 * with a JSON schema, inserts via SQL, selects JSON rows, runs Arrow IPC,
 * migrates, and uses the Kit query builder - all in-process.
 */

/* Use the Kit FFI header, NOT the HTTP client header. */
#include "mongreldb_kit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CHECK(call) do { \
    int32_t _rc = (call); \
    if (_rc != 0) { \
        fprintf(stderr, "FAIL: %s returned %d: %s\n", #call, _rc, \
                mongreldb_kit_last_error()); \
        exit(1); \
    } \
} while(0)

static const char *SCHEMA_JSON =
    "{\"tables\":[{\"id\":1,\"name\":\"users\","
    "\"columns\":["
    "{\"id\":1,\"name\":\"id\",\"storage_type\":\"int64\",\"application_type\":\"int64\",\"nullable\":false,\"primary_key\":true,\"default\":null,\"generated\":false},"
    "{\"id\":2,\"name\":\"name\",\"storage_type\":\"text\",\"application_type\":\"text\",\"nullable\":true,\"primary_key\":false,\"default\":null,\"generated\":false},"
    "{\"id\":3,\"name\":\"email\",\"storage_type\":\"text\",\"application_type\":\"text\",\"nullable\":true,\"primary_key\":false,\"default\":null,\"generated\":false}"
    "],\"primary_key\":[\"id\"]}]}";

int main(void) {
    char dbdir[256];
    snprintf(dbdir, sizeof(dbdir), "/tmp/mdb_native_c_%d", (int)getpid());
    mkdir(dbdir, 0755);

    printf("=== Native Embedded Basic CRUD (C + Kit FFI) ===\n");
    printf("Database dir: %s\n\n", dbdir);

    /* 1. Create the Kit database with a JSON schema. */
    mongreldb_kit_database_t *db = mongreldb_kit_create(dbdir, SCHEMA_JSON);
    if (!db) {
        fprintf(stderr, "create failed: %s\n", mongreldb_kit_last_error());
        return 1;
    }
    printf("1. Database created with schema (users table)\n");

    /* 2. Insert rows via SQL. */
    const char *json = NULL;
    CHECK(mongreldb_kit_sql_rows(db,
        "INSERT INTO users (id, name, email) VALUES (1, 'Alice', 'alice@example.com')",
        &json));
    mongreldb_kit_free_json((char *)json);

    CHECK(mongreldb_kit_sql_rows(db,
        "INSERT INTO users (id, name, email) VALUES (2, 'Bob', 'bob@example.com')",
        &json));
    mongreldb_kit_free_json((char *)json);

    CHECK(mongreldb_kit_sql_rows(db,
        "INSERT INTO users (id, name, email) VALUES (3, 'Carol', 'carol@example.com')",
        &json));
    mongreldb_kit_free_json((char *)json);
    printf("2. Inserted 3 rows via SQL\n");

    /* 3. SELECT via SQL (JSON rows). */
    CHECK(mongreldb_kit_sql_rows(db,
        "SELECT id, name, email FROM users ORDER BY id", &json));
    printf("3. SELECT all rows:\n   %s\n", json);
    mongreldb_kit_free_json((char *)json);

    /* 4. Arrow IPC for columnar reads. */
    uint8_t *arrow = NULL;
    size_t arrow_len = 0;
    CHECK(mongreldb_kit_sql_arrow(db, "SELECT id FROM users", &arrow, &arrow_len));
    printf("4. Arrow IPC: %zu bytes", arrow_len);
    if (arrow_len >= 6)
        printf(", magic: %.6s", arrow);
    printf("\n");
    mongreldb_kit_free_arrow(arrow, arrow_len);

    /* 5. Migration: add an orders table. */
    CHECK(mongreldb_kit_migrate_json(db,
        "[{\"version\":1,\"name\":\"add_orders\","
        "\"ops\":[{\"raw_sql\":\"CREATE TABLE orders (id INT64 PRIMARY KEY, user_id INT64, total FLOAT64)\"}]}]"));
    printf("5. Migration: created 'orders' table\n");

    /* Insert into the migrated table. */
    CHECK(mongreldb_kit_sql_rows(db,
        "INSERT INTO orders (id, user_id, total) VALUES (1, 1, 99.99)", &json));
    mongreldb_kit_free_json((char *)json);
    CHECK(mongreldb_kit_sql_rows(db,
        "INSERT INTO orders (id, user_id, total) VALUES (2, 2, 49.99)", &json));
    mongreldb_kit_free_json((char *)json);

    /* 6. SQL JOIN across both tables. */
    CHECK(mongreldb_kit_sql_rows(db,
        "SELECT u.name, o.total FROM users u "
        "JOIN orders o ON u.id = o.user_id ORDER BY o.total DESC", &json));
    printf("6. SQL JOIN (users + orders):\n   %s\n", json);
    mongreldb_kit_free_json((char *)json);

    /* 7. Kit query builder: SELECT. */
    CHECK(mongreldb_kit_query_select_json(db,
        "{\"table\":\"users\",\"columns\":[],\"filter\":null,\"order_by\":[],\"limit\":null,\"offset\":null}",
        &json));
    printf("7. Kit query builder SELECT:\n   %s\n", json);
    mongreldb_kit_free_json((char *)json);

    /* 8. Read back applied migrations. */
    CHECK(mongreldb_kit_applied_migrations_json(db, &json));
    printf("8. Applied migrations: %s\n", json);
    mongreldb_kit_free_json((char *)json);

    /* Cleanup. */
    mongreldb_kit_database_free(db);
    printf("\n=== All operations completed successfully! ===\n");
    return 0;
}
