<p align="center">
  <img src="assets/mongrel.png" alt="MongrelDB logo" width="250" />
</p>

<h1 align="center">MongrelDB C Client</h1>

<p align="center">
  <b>Pure C99 HTTP client for MongrelDB - embedded+server database with SQL, vector search, full-text search, and AI-native retrieval.</b>
  <br />
  Built on libcurl and a bundled JSON parser. No external runtime dependencies beyond libc and libcurl. Also bundles the engine's C ABI header for native embedding.
</p>

<p align="center">
  <a href="https://github.com/visorcraft/MongrelDB-C/actions/workflows/ci.yml"><img src="https://github.com/visorcraft/MongrelDB-C/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/visorcraft/MongrelDB/releases"><img src="https://img.shields.io/badge/server-v0.64.3-blue.svg" alt="MongrelDB server" /></a>
  <a href="#license"><img src="https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue.svg" alt="License" /></a>
</p>

## Package

| Surface | Package | Install |
|---|---|---|
| C client | `MongrelDB-C` | build from source with CMake + libcurl |

History retention: `mongreldb_history_retention_get` and
`mongreldb_history_retention_set` expose `GET`/`PUT /history/retention`.

## Requirements

- **A C99 compiler** (gcc, clang, MSVC)
- **libcurl** (the HTTP transport). On Debian/Ubuntu install `libcurl4-openssl-dev`; on Fedora `curl-devel`; on macOS it ships with the system.
- **CMake 3.16 or newer** (to build)
- A running [`mongreldb-server`](https://github.com/visorcraft/MongrelDB) daemon

## What It Provides

- **Typed CRUD** over the Kit transaction endpoint: `mongreldb_put`, `mongreldb_upsert` (insert-or-update on PK conflict), `mongreldb_delete` by row id or primary key, with idempotency keys for safe retries.
- **Query builder** that pushes conditions down to the engine's specialized indexes for sub-millisecond lookups: bitmap equality, learned-range, null checks, and FM-index full-text search. Conditions are AND-ed.
- **Idempotent batch transactions** - all operations staged locally and committed atomically, with the engine enforcing unique, foreign key, and check constraints at commit time. Idempotency keys return the original response on duplicate commits, even after a crash.
- **Full SQL access** through the DataFusion-backed `/sql` endpoint: recursive CTEs, window functions, `CREATE TABLE AS SELECT`, materialized views, and multi-statement execution.
- **Schema management**: typed table creation, full schema catalog, and per-table descriptors.
- **Native Tier-1 embedding**: bundle the C ABI headers (`mongreldb_engine.h` + `mongreldb_kit.h`) and link `libmongreldb` + `libmongreldb_kit` for in-process access with zero serialization overhead. SQL, migrations, and the query builder are all available through the FFI. Download prebuilt libraries from the [MongrelDB releases](https://github.com/visorcraft/MongrelDB/releases) page (`mongreldb-native-*` and `mongreldb-kit-native-*` archives).
- **Typed error codes**: `MDB_ERR_AUTH` (401/403), `MDB_ERR_NOT_FOUND` (404), `MDB_ERR_CONFLICT` (409), `MDB_ERR_QUERY` (400/5xx), plus `MDB_ERR_NETWORK`, `MDB_ERR_JSON`, and `MDB_ERR_NOMEM`. Retrieve the detail with `mongreldb_last_error()`.
- **Zero external JSON dependency**: a minimal parser is included, so the only runtime dependency beyond libc is libcurl.

## Examples

Runnable, commented examples live in the docs:

- [Quickstart](docs/quickstart.md) - install, start the daemon, write and run a complete program.
- [Transactions](docs/transactions.md) - batch commits, idempotency keys, constraint handling.
- [Queries](docs/queries.md) - every native condition type and the index it pushes down to.
- [SQL](docs/sql.md) - recursive CTEs, window functions, advanced SQL.
- [Authentication](docs/auth.md) - bearer token, HTTP Basic, and open modes.
- [Errors](docs/errors.md) - error codes, the HTTP-status mapping, and recovery patterns.

## Quick Example

```c
#include <mongreldb.h>
#include <stdio.h>

int main(void) {
    /* Connect to a running mongreldb-server daemon. */
    mongreldb_client *db = mongreldb_connect("http://127.0.0.1:8453");

    /* Create a table. Column ids are stable on-wire identifiers. */
    mongreldb_column cols[] = {
        {1, "id",       "int64",   /*primary_key=*/1, /*nullable=*/0},
        {2, "customer", "varchar", /*primary_key=*/0, /*nullable=*/0},
        {3, "amount",   "float64", /*primary_key=*/0, /*nullable=*/0},
    };
    int64_t tid = 0;
    mongreldb_create_table(db, "orders", cols, 3, &tid);

    /* Insert rows (cells pair column id + value). */
    mongreldb_input_cell r1[] = {
        {1, {MDB_VAL_INT64,  .v.i64 = 1}},
        {2, {MDB_VAL_STRING, .v.str = "Alice"}},
        {3, {MDB_VAL_DOUBLE, .v.f64 = 99.50}},
    };
    mongreldb_input_cell r2[] = {
        {1, {MDB_VAL_INT64,  .v.i64 = 2}},
        {2, {MDB_VAL_STRING, .v.str = "Bob"}},
        {3, {MDB_VAL_DOUBLE, .v.f64 = 150.00}},
    };
    mongreldb_put(db, "orders", r1, 3, NULL);
    mongreldb_put(db, "orders", r2, 3, NULL);

    /* Query with a native index condition (learned-range index). */
    mongreldb_condition cond = {
        .kind = MDB_COND_RANGE,
        .column_id = 3,
        .lo = 100.0, .lo_set = 1,
    };
    mongreldb_result res;
    mongreldb_query(db, "orders", &cond, 1, NULL, 0, 100, &res);
    printf("rows: %zu\n", res.count);

    int64_t n = 0;
    mongreldb_count(db, "orders", &n);
    printf("count: %lld\n", (long long)n); /* 2 */

    /* Run SQL. */
    mongreldb_sql(db, "UPDATE orders SET amount = 200.0 WHERE customer = 'Bob'", NULL);

    mongreldb_close(db);
    return 0;
}
```

## Authentication

```c
/* Bearer token (--auth-token mode) */
mongreldb_client *db = mongreldb_connect_with_token(
    "http://127.0.0.1:8453", "my-secret-token");

/* HTTP Basic (--auth-users mode) */
mongreldb_client *db = mongreldb_connect_with_basic_auth(
    "http://127.0.0.1:8453", "admin", "s3cret");
```

A token takes precedence over basic auth if both are supplied.

## History retention

`mongreldb_history_retention_get` and `mongreldb_history_retention_set` expose
`GET /history/retention` and `PUT /history/retention`. They inspect and change
the rolling MVCC history window, measured in committed epochs:

```c
mongreldb_history_retention ret;

int rc = mongreldb_history_retention_get(db, &ret);
printf("retain %llu epochs; earliest retained epoch %llu\n",
       (unsigned long long)ret.history_retention_epochs,
       (unsigned long long)ret.earliest_retained_epoch);

rc = mongreldb_history_retention_set(db, 4096, &ret);
```

When catalog authentication is enabled, both routes require the `ADMIN`
permission. Increasing the retention window cannot restore history that was
already pruned; the wider guarantee only applies from the current epoch forward.

## Batch transactions

Operations are staged locally and committed atomically. The engine enforces
unique, foreign key, and check constraints at commit time.

```c
mongreldb_input_cell a[] = {{1, {MDB_VAL_INT64,  .v.i64 = 10}},
                            {2, {MDB_VAL_STRING, .v.str = "Dave"}},
                            {3, {MDB_VAL_DOUBLE, .v.f64 = 50.00}}};
mongreldb_input_cell b[] = {{1, {MDB_VAL_INT64,  .v.i64 = 11}},
                            {2, {MDB_VAL_STRING, .v.str = "Eve"}},
                            {3, {MDB_VAL_DOUBLE, .v.f64 = 75.00}}};
mongreldb_value pk = {MDB_VAL_INT64, .v.i64 = 2};

mongreldb_op ops[3];
memset(ops, 0, sizeof(ops));
ops[0].type = MDB_OP_PUT;       ops[0].table = "orders";
ops[0].cells = a;               ops[0].cell_count = 3;
ops[1].type = MDB_OP_PUT;       ops[1].table = "orders";
ops[1].cells = b;               ops[1].cell_count = 3;
ops[2].type = MDB_OP_DELETE_BY_PK;
ops[2].table = "orders";        ops[2].pk_value = pk;

/* Atomic - all or nothing. The idempotency key makes it safe to retry. */
int rc = mongreldb_commit(db, ops, 3, "batch-1");
if (rc == MDB_ERR_CONFLICT) {
    fprintf(stderr, "constraint violated: %s\n", mongreldb_last_error(db));
}
```

## Native query builder

Conditions push down to the engine's specialized indexes. Each `mongreldb_condition`
targets one index; multiple conditions are AND-ed.

```c
/* Bitmap equality (low-cardinality columns) */
mongreldb_condition bitmap = {
    .kind = MDB_COND_BITMAP_EQ, .column_id = 2, .str_value = "Alice"};

/* Range query (learned-range index) */
mongreldb_condition range = {
    .kind = MDB_COND_RANGE, .column_id = 3,
    .lo = 50.0, .lo_set = 1, .hi = 150.0, .hi_set = 1};

/* Full-text search (FM-index) */
mongreldb_condition fts = {
    .kind = MDB_COND_FM_CONTAINS, .column_id = 2,
    .str_value = "database performance"};

mongreldb_condition conds[] = {bitmap, range};
int64_t proj[] = {1, 3};
mongreldb_result res;
mongreldb_query(db, "orders", conds, 2, proj, 2, 100, &res);
if (res.truncated) {
    /* result set hit the limit; more matches exist on the server */
}
```

For `ann`, `sparse_match`, `minhash_similar_members`, `bitmap_in`, and every
other complete server condition, set `condition_json` to the externally tagged
JSON object. Use `MDB_VAL_JSON` for embedding vectors, sparse pairs, sets,
arrays, and object cell values.

## Schema constraints

Optional fields on `mongreldb_column` let you constrain what goes into a column
at create time. They are omitted from the wire JSON when left unset, so existing
schemas are unaffected.

```c
/* A varchar column whose values must come from this fixed set.
 * The wire emit is "enum_variants": ["a","b","c"]. */
static const char *const kStatusVariants[] = {"active", "inactive", "paused"};
mongreldb_column cols[] = {
    {1, "id",         "int64",   /*primary_key=*/1, /*nullable=*/0},
    {2, "customer",   "varchar", /*primary_key=*/0, /*nullable=*/0},
    {3, "created_at", "timestamp_nanos", /*primary_key=*/0, /*nullable=*/0,
        .default_expr = "now"},
    {4, "status",     "varchar", /*primary_key=*/0, /*nullable=*/0,
        .enum_variants = kStatusVariants, .enum_variants_len = 3,
        .default_value_json = "\"active\""},
    {5, "attempts",   "int64",   /*primary_key=*/0, /*nullable=*/0,
        .default_value_json = "0"},
    {6, "enabled",    "bool",    /*primary_key=*/0, /*nullable=*/0,
        .default_value_json = "true"},
};
```

`enum_variants` is a `const char *const *` plus a length; both NULL/0 means
"absent". `default_value_json` sends a caller-validated raw JSON scalar such as
`"\"draft\""`, `"7"`, `"true"`, or `"null"`, preserving the literal JSON type on
the wire. `default_expr` sends a dynamic default such as `"now"` or `"uuid"` and
takes precedence over static defaults. The legacy `default_value` field still
sends a plain string when used. Because these fields extend the public
`mongreldb_column` struct, rebuild consumers when upgrading. The constraint is
enforced server-side, so a row whose value falls outside the listed variants
surfaces as `MDB_ERR_CONFLICT` on `put`/`commit`.

Table CHECKs use the additive constraints overload. The JSON is the daemon's
native `constraints` object:

```c
const char *constraints =
    "{\"checks\":[{\"id\":1,\"name\":\"amount_nonneg\",\"expr\":"
    "{\"Ge\":[{\"Col\":3},{\"Lit\":{\"Float64\":0.0}}]}}]}";
mongreldb_create_table_with_constraints_json(db, "orders", cols, 3,
                                              constraints, &tid);
```

## SQL

```c
mongreldb_sql(db, "INSERT INTO orders (id, customer, amount) VALUES (99, 'Zoe', 999.0)", NULL);
mongreldb_sql(db, "CREATE TABLE archive AS SELECT * FROM orders WHERE amount > 500", NULL);

/* Recursive CTEs and window functions */
mongreldb_sql(db,
    "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM r WHERE n<10) "
    "SELECT n FROM r", NULL);
mongreldb_sql(db,
    "SELECT id, ROW_NUMBER() OVER (PARTITION BY customer ORDER BY amount DESC) "
    "FROM orders", NULL);
```

## User & role management

User, role, and permission management is performed through SQL against the
daemon's catalog. Passwords are Argon2id-hashed server-side.

```c
mongreldb_sql(db, "CREATE USER admin WITH PASSWORD 's3cret-pw'", NULL);
mongreldb_sql(db, "ALTER USER admin SET ADMIN TRUE", NULL);

mongreldb_sql(db, "CREATE ROLE analyst", NULL);
mongreldb_sql(db, "GRANT select ON orders TO analyst", NULL);  /* table-level */
mongreldb_sql(db, "GRANT analyst TO alice", NULL);

mongreldb_sql(db, "SELECT username FROM catalog.users", NULL);  /* list users */
mongreldb_sql(db, "SELECT name FROM catalog.roles", NULL);      /* list roles */
```

## Error handling

Every function returns an `int` error code. `MDB_OK` (0) means success;
negative values are failures. Use `mongreldb_last_error()` for the message.

```c
int rc = mongreldb_schema_for(db, "missing_table", &body);
switch (rc) {
case MDB_OK:
    break;
case MDB_ERR_NOT_FOUND:
    fprintf(stderr, "not found: %s\n", mongreldb_last_error(db));
    break;
case MDB_ERR_CONFLICT:
    fprintf(stderr, "constraint: %s\n", mongreldb_last_error(db));
    break;
case MDB_ERR_AUTH:
    fprintf(stderr, "not authorized: %s\n", mongreldb_last_error(db));
    break;
case MDB_ERR_NETWORK:
    fprintf(stderr, "can't reach daemon: %s\n", mongreldb_last_error(db));
    break;
default:
    fprintf(stderr, "error: %s\n", mongreldb_last_error(db));
    break;
}
```

## API reference

### Client lifecycle

| Function | Description |
|----------|-------------|
| `mongreldb_connect(url)` | Construct a client (NULL url defaults to `http://127.0.0.1:8453`) |
| `mongreldb_connect_with_token(url, token)` | Bearer token auth (`--auth-token` mode) |
| `mongreldb_connect_with_basic_auth(url, user, pass)` | HTTP Basic auth (`--auth-users` mode) |
| `mongreldb_set_timeout(c, seconds)` | Per-request timeout (default 30) |
| `mongreldb_close(c)` | Release the client and all owned memory |
| `mongreldb_last_error(c)` | Message for the most recent failure (valid until next call) |
| `mongreldb_last_error_code(c)` | Code for the most recent failure |

### Database operations

| Function | Description |
|----------|-------------|
| `mongreldb_health(c)` | Check daemon health |
| `mongreldb_table_names(c, &names, &count)` | List table names |
| `mongreldb_create_table(c, name, cols, n, &tid)` | Create a table; column descriptors may carry enum/default fields |
| `mongreldb_create_table_with_constraints_json(c, name, cols, n, json, &tid)` | Create a table with native `constraints` JSON (including CHECKs) |
| `mongreldb_create_table_with_schema_json(c, name, cols, n, constraints, indexes, count, &tid)` | Create a table with all six index kinds and options |
| `mongreldb_history_retention_get(c, &ret)` | Inspect the MVCC history retention window |
| `mongreldb_history_retention_set(c, epochs, &ret)` | Resize the MVCC history retention window |
| `mongreldb_drop_table(c, name)` | Drop a table |
| `mongreldb_count(c, table, &n)` | Row count |
| `mongreldb_put(c, table, cells, n, key)` | Insert a row |
| `mongreldb_upsert(c, table, cells, n, upd, un, key)` | Upsert a row |
| `mongreldb_delete(c, table, row_id)` | Delete by row id |
| `mongreldb_delete_by_pk(c, table, &pk)` | Delete by primary key |
| `mongreldb_commit(c, ops, n, key)` | Commit a batch atomically |
| `mongreldb_query(c, table, conds, n, proj, pn, limit, &res)` | Run a native query |
| `mongreldb_query_page(c, table, conds, n, proj, pn, limit, offset, &res)` | Run a paged native query |
| `mongreldb_sql(c, sql, &body)` | Execute SQL |
| `mongreldb_schema(c, &body)` | Full schema catalog (raw JSON) |
| `mongreldb_schema_for(c, table, &body)` | Single-table descriptor (raw JSON) |

### Bundled engine ABI

`include/mongreldb_engine.h` is a verbatim copy of the engine's C ABI for users
who link the engine natively (via `libmongreldb`). See the comments in that
header for the handle-based put/query/transaction/auth surface.

## Building and testing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run the live test suite (boots mongreldb-server itself if it can find the
# binary in this order: MONGRELDB_SERVER env var, ./bin/mongreldb-server,
# or mongreldb-server on PATH). Set MONGRELDB_URL to use an already-running
# daemon. Tests self-skip when no binary is available.
ctest --test-dir build --output-on-failure
```

Fetch a prebuilt server binary from the [MongrelDB releases](https://github.com/visorcraft/MongrelDB/releases):

```sh
mkdir -p bin
curl -fsSL -o bin/mongreldb-server \
  https://github.com/visorcraft/MongrelDB/releases/download/v0.64.3/mongreldb-server-linux-x64
chmod +x bin/mongreldb-server
```

### Linking against the client

```cmake
# In your CMakeLists.txt
find_package(CURL REQUIRED)
add_subdirectory(mongreldb_c)
target_link_libraries(your_app PRIVATE mongreldb_c)
```

Or compile the sources directly:

```sh
cc -std=c99 -I/path/to/MongrelDB-C/include your_app.c \
   /path/to/MongrelDB-C/src/mongreldb.c -lcurl -o your_app
```

## Native embedding (Tier 1)

For in-process access with zero serialization overhead, link the prebuilt
`libmongreldb` (core engine) and optionally `libmongreldb_kit` (schema model,
migrations, query builder) instead of connecting to a daemon. Download the
prebuilt libraries from the
[MongrelDB releases](https://github.com/visorcraft/MongrelDB/releases) page:

```sh
# Download for your platform, e.g. linux-x64-gnu
curl -fsSL -o native.tar.gz \
  https://github.com/visorcraft/MongrelDB/releases/download/v0.64.3/mongreldb-native-linux-x64-gnu.tar.gz
tar xzf native.tar.gz  # produces mongreldb-native/{lib,include}/

curl -fsSL -o kit-native.tar.gz \
  https://github.com/visorcraft/MongrelDB/releases/download/v0.64.3/mongreldb-kit-native-linux-x64-gnu.tar.gz
tar xzf kit-native.tar.gz  # produces mongreldb-kit-native/{lib,include}/
```

Then compile against the bundled headers (`mongreldb_engine.h` for the core
ABI, `mongreldb_kit.h` for the Kit layer):

```sh
# Core engine: create database, put rows, run SQL
cc -std=c99 -Iinclude -Imongreldb-native/include your_engine_app.c \
   -Lmongreldb-native/lib -lmongreldb -lpthread -ldl -lm \
   -Wl,-rpath,mongreldb-native/lib -o your_engine_app

# Kit layer: schema model, migrations, query builder
cc -std=c99 -Iinclude -Imongreldb-native/include -Imongreldb-kit-native/include \
   your_kit_app.c \
   -Lmongreldb-kit-native/lib -lmongreldb_kit \
   -Lmongreldb-native/lib -lmongreldb \
   -lpthread -ldl -lm \
   -Wl,-rpath,mongreldb-native/lib -Wl,-rpath,mongreldb-kit-native/lib \
   -o your_kit_app
```

The core ABI header `mongreldb_engine.h` and Kit header `mongreldb_kit.h` are
bundled in the `include/` directory. **Do not include `mongreldb.h` (the HTTP
client header) and `mongreldb_engine.h` in the same translation unit** - they
declare conflicting `mongreldb_*` symbols. Use exactly one per `.c` file.

See the FFI crate's [`docs/migrations.md`](https://github.com/visorcraft/MongrelDB/blob/master/crates/mongreldb-ffi/docs/migrations.md)
for the full `MigrationOp` to FFI call mapping when running migrations via the
native ABI.

## Contributing

Contributions are welcome. Please:

1. Open an issue first for non-trivial changes.
2. Add focused tests near your change - the suite must stay green.
3. Keep the code C99, warning-clean under `-Wall -Wextra -Wpedantic`.
4. Match the existing style: 4-space indent, snake_case, header guards.

## License

Dual-licensed under the **MIT License** or the **Apache License, Version 2.0**,
at your option. See [MIT](LICENSE-MIT) OR [Apache-2.0](LICENSE-APACHE) for the full text.

`SPDX-License-Identifier: MIT OR Apache-2.0`
