# Quickstart

Zero to a running MongrelDB C program in fifteen minutes. This guide assumes a
fresh machine and walks through installing the prerequisites, starting the
daemon, and writing, running, and understanding a complete program.

---

## 1. Prerequisites

You need three things installed: a C99 compiler, libcurl (with headers), CMake,
and a `mongreldb-server` daemon.

### Install a C compiler, libcurl, and CMake

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libcurl4-openssl-dev
```

On Fedora:

```sh
sudo dnf install gcc cmake libcurl-devel
```

On macOS, the Xcode Command Line Tools provide clang and libcurl; install CMake
via Homebrew (`brew install cmake`).

Verify:

```sh
cc --version
pkg-config --modversion libcurl   # 8.x
cmake --version                   # >= 3.16
```

### Install mongreldb-server

Fetch a prebuilt server binary from the
[MongrelDB releases](https://github.com/visorcraft/MongrelDB/releases):

```sh
mkdir -p bin
curl -fsSL -o bin/mongreldb-server \
  https://github.com/visorcraft/MongrelDB/releases/download/v0.46.2/mongreldb-server-linux-x64
chmod +x bin/mongreldb-server
```

Verify it runs:

```sh
./bin/mongreldb-server --version
```

## 2. Start the daemon

By default `mongreldb-server` listens on `http://127.0.0.1:8453` and stores
data in the directory you pass as its first argument.

```sh
mkdir -p /tmp/mdb-data
/path/to/mongreldb-server /tmp/mdb-data
```

In another terminal, sanity-check it:

```sh
curl http://127.0.0.1:8453/health
# ok
```

Leave the daemon running for the rest of this guide.

## 3. Create a project and build the client

```sh
mkdir mdb-demo && cd mdb-demo
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 4. Write your first program

Create `demo.c`:

```c
#include <mongreldb.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* 1. Connect to the daemon. NULL falls back to http://127.0.0.1:8453. */
    mongreldb_client *db = mongreldb_connect(NULL);

    /* 2. Health check before doing anything else. */
    if (mongreldb_health(db) != MDB_OK) {
        fprintf(stderr, "daemon not reachable: %s\n", mongreldb_last_error(db));
        return 1;
    }

    /* 3. Create a table. Each column has a stable numeric id, a name, a type,
     *    and flags. The first column is the primary key. */
    mongreldb_column cols[] = {
        {1, "id",       "int64",   /*primary_key=*/1, /*nullable=*/0},
        {2, "customer", "varchar", /*primary_key=*/0, /*nullable=*/0},
        {3, "amount",   "float64", /*primary_key=*/0, /*nullable=*/0},
    };
    int64_t tid = 0;
    if (mongreldb_create_table(db, "orders", cols, 3, &tid) != MDB_OK) {
        fprintf(stderr, "create table: %s\n", mongreldb_last_error(db));
        return 1;
    }

    /* 4. Insert rows. Cells pair column id + value. */
    mongreldb_input_cell r1[] = {
        {1, {MDB_VAL_INT64,  .v.i64 = 1}},
        {2, {MDB_VAL_STRING, .v.str = "Alice"}},
        {3, {MDB_VAL_DOUBLE, .v.f64 = 99.5}},
    };
    mongreldb_input_cell r2[] = {
        {1, {MDB_VAL_INT64,  .v.i64 = 2}},
        {2, {MDB_VAL_STRING, .v.str = "Bob"}},
        {3, {MDB_VAL_DOUBLE, .v.f64 = 150.0}},
    };
    mongreldb_put(db, "orders", r1, 3, NULL);
    mongreldb_put(db, "orders", r2, 3, NULL);

    /* 5. Query with a native index condition. The range index serves this in
     *    sub-millisecond. Projection selects only column ids 1 and 2. */
    mongreldb_condition cond = {
        .kind = MDB_COND_RANGE, .column_id = 3,
        .lo = 100.0, .lo_set = 1,
    };
    int64_t proj[] = {1, 2};
    mongreldb_result res;
    if (mongreldb_query(db, "orders", &cond, 1, proj, 2, 100, &res) != MDB_OK) {
        fprintf(stderr, "query: %s\n", mongreldb_last_error(db));
        return 1;
    }
    for (size_t i = 0; i < res.count; i++) {
        for (size_t j = 0; j < res.rows[i].count; j++) {
            printf("col %lld: ", (long long)res.rows[i].cells[j].column_id);
            /* (print the value by tag here) */
        }
        printf("\n");
    }

    /* 6. Count the rows. */
    int64_t n = 0;
    mongreldb_count(db, "orders", &n);
    printf("total rows: %lld\n", (long long)n);

    mongreldb_close(db);
    return 0;
}
```

Build and run it:

```sh
cc -std=c99 -Iinclude demo.c src/mongreldb.c $(pkg-config --cflags --libs libcurl) -o demo
./demo
```

You should see the row count of 2.

## 5. What each part does

| Code | What it does |
|------|--------------|
| `mongreldb_connect(url)` | Builds an HTTP client targeting one daemon. One per thread. |
| `mongreldb_health(c)` | GET `/health`; returns `MDB_OK` when the daemon answers. Always check before real work. |
| `mongreldb_create_table(c, name, cols, n, &tid)` | POST `/kit/create_table`. Column `id`s are the on-wire identifiers; use them everywhere else. |
| `mongreldb_put(c, table, cells, n, key)` | Single-op transaction: POST `/kit/txn` with one `put` op. `cells` is flattened to `[col_id, val, ...]`. |
| `mongreldb_query(...)` | Builds a `/kit/query` body. Conditions push down to native indexes. |
| `.projection = {1,2}` | Server returns only those column ids, saving bandwidth. |
| `.limit = 100` | Caps the result; check `res.truncated` afterward to detect overflow. |
| `mongreldb_count(c, table, &n)` | GET `/tables/{name}/count`. |

## 6. Common pitfalls

**Using the column name instead of the column id.** Every on-wire API uses the
numeric `id` from `create_table`, never the `name`. Conditions take the int64
`column_id`, not the string name.

**Treating a single `mongreldb_put` as non-transactional.** `put` is a one-op
transaction. A unique constraint violation surfaces as `MDB_ERR_CONFLICT` (HTTP
409), not as a silent no-op.

**Reading result memory after the next call.** Result rows, values, and table
lists are owned by the client and valid only until the next call (or
`mongreldb_close()`). Copy anything you need to keep into your own storage.

**Expecting `mongreldb_sql` to always return rows.** The `/sql` endpoint
streams Arrow IPC for `SELECT` in most builds, so `sql` returns `MDB_OK` with
the raw (possibly non-JSON) body in `out_body`. Use it for DDL/DML and
statements whose success is the signal; use the native query builder for typed
row retrieval.

**Pointing at a daemon that requires auth.** If the daemon was started with
`--auth-token` or `--auth-users`, every call fails with `MDB_ERR_AUTH` unless
you use `mongreldb_connect_with_token(...)` or
`mongreldb_connect_with_basic_auth(...)`. See [auth.md](auth.md).

## Next steps

- [transactions.md](transactions.md) - atomic batches, idempotency, retries
- [queries.md](queries.md) - every native index condition
- [sql.md](sql.md) - recursive CTEs, window functions, `CREATE TABLE AS SELECT`
- [auth.md](auth.md) - bearer tokens, basic auth, user/role management
- [errors.md](errors.md) - the full error code set and recovery patterns
