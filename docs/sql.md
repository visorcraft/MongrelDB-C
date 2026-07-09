# SQL

MongrelDB ships a DataFusion-backed SQL engine at `POST /sql`. From C, run SQL
with `mongreldb_sql`:

```c
const char *body = NULL;
mongreldb_sql(db, "SELECT 1", &body);
```

This guide covers the SQL surface - DDL, DML, `CREATE TABLE AS SELECT`,
recursive CTEs, and window functions - and when to reach for SQL versus the
native query builder.

---

## How `mongreldb_sql` behaves

`mongreldb_sql(c, sql, &body)` sends `{"sql": "..."}` to `/sql`. It returns
`MDB_OK` on a 2xx response. When `out_body` is non-NULL, it receives the raw
response body (NUL-terminated, client-owned, valid until the next call).

In practice:

- **DDL and DML** (`CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`) reply with a
  non-JSON status body. `mongreldb_sql` returns `MDB_OK` - success is the
  signal.
- **`SELECT`** in most daemon builds streams Arrow IPC bytes rather than JSON.
  Use the native `mongreldb_query` for typed row retrieval in application code,
  and use SQL for statements whose execution is the goal (DDL/DML/admin).

Errors are mapped to the same error codes as everything else: an HTTP 400 or
5xx is `MDB_ERR_QUERY`; 409 is `MDB_ERR_CONFLICT`; and so on. See
[errors.md](errors.md).

```c
if (mongreldb_sql(db,
        "INSERT INTO orders (id, customer, amount) VALUES (99, 'Zoe', 999.0)",
        NULL) == MDB_ERR_CONFLICT) {
    fprintf(stderr, "duplicate row: %s\n", mongreldb_last_error(db));
}
```

## CREATE TABLE

Define a table in SQL instead of via `mongreldb_create_table`. Column ids are
assigned by the server when not stated.

```c
mongreldb_sql(db,
    "CREATE TABLE products ("
    "  id INT64 PRIMARY KEY,"
    "  name VARCHAR,"
    "  price FLOAT64,"
    "  category VARCHAR,"
    "  in_stock BOOLEAN"
    ")",
    NULL);
```

## INSERT

```c
mongreldb_sql(db,
    "INSERT INTO products (id, name, price, category, in_stock) "
    "VALUES (1, 'Widget', 9.99, 'tools', true)", NULL);
mongreldb_sql(db,
    "INSERT INTO products VALUES (2, 'Gadget', 19.99, 'tools', true)", NULL);
```

For bulk inserts, the native batch transaction (`mongreldb_commit`) is usually
faster because it stages ops in one round trip without re-parsing SQL.

## UPDATE

```c
mongreldb_sql(db, "UPDATE products SET price = 14.99 WHERE id = 1", NULL);
mongreldb_sql(db, "UPDATE orders SET amount = 200.0 WHERE customer = 'Bob'", NULL);
```

## DELETE

```c
mongreldb_sql(db, "DELETE FROM products WHERE in_stock = false", NULL);
mongreldb_sql(db, "DELETE FROM products WHERE id = 2", NULL);
```

## SELECT

```c
mongreldb_sql(db, "SELECT id, name FROM products WHERE category = 'tools' ORDER BY price", NULL);
mongreldb_sql(db, "SELECT category, COUNT(*) AS n FROM products GROUP BY category", NULL);
```

Remember SELECT bodies usually arrive as Arrow IPC, so `mongreldb_sql` returns
the raw body. To read rows back into typed values, mirror the same lookup with
`mongreldb_query`.

## CREATE TABLE AS SELECT

Materialize a query result into a new table. Great for snapshots, rollups, and
denormalized aggregates.

```c
/* Snapshot all high-value orders into a new table. */
mongreldb_sql(db, "CREATE TABLE archive AS SELECT * FROM orders WHERE amount > 500", NULL);

/* Roll up sales by customer. */
mongreldb_sql(db,
    "CREATE TABLE sales_by_customer AS "
    "SELECT customer, SUM(amount) AS total FROM orders GROUP BY customer",
    NULL);
```

The new table inherits column types from the query. Query it afterward with
the native builder or SQL.

## Recursive CTEs

`WITH RECURSIVE` is fully supported. Classic use cases: series generation,
hierarchy/graph traversal.

```c
/* Generate the numbers 1..10. */
mongreldb_sql(db,
    "WITH RECURSIVE r(n) AS ("
    "  SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 10"
    ") SELECT n FROM r",
    NULL);
```

A common practical example is walking an adjacency list:

```c
mongreldb_sql(db,
    "WITH RECURSIVE descendants(id) AS ("
    "  SELECT id FROM categories WHERE id = 1"
    "  UNION ALL"
    "  SELECT c.id FROM categories c JOIN descendants d ON c.parent_id = d.id"
    ") SELECT id FROM descendants",
    NULL);
```

## Window functions

Window functions compute aggregates/rankings across a moving window without
collapsing rows. Useful for top-N-per-group, running totals, and row numbers.

```c
/* Row number within each customer, ordered by amount descending. */
mongreldb_sql(db,
    "SELECT id, customer, amount, "
    "ROW_NUMBER() OVER (PARTITION BY customer ORDER BY amount DESC) AS rn "
    "FROM orders",
    NULL);

/* Running total per customer. */
mongreldb_sql(db,
    "SELECT id, customer, amount, "
    "SUM(amount) OVER (PARTITION BY customer ORDER BY id) AS running_total "
    "FROM orders",
    NULL);
```

`RANK()`, `DENSE_RANK()`, `LAG()`, `LEAD()`, `NTILE()`, and the usual
window-frame clauses are available through DataFusion.

## When to use SQL vs. the query builder

Both read from the same tables, but they are optimized for different jobs.

| Reach for | When |
|-----------|------|
| **`mongreldb_query`** | Point lookups, range scans, bitmap filters, and full-text that map to a native index. Sub-millisecond, no parser overhead, and rows decode into typed values directly. |
| **SQL** | DDL (`CREATE TABLE`, schemas, materialized views), multi-statement setup, joins, recursive CTEs, window functions, and arbitrary aggregates. Also the natural choice for admin scripts and one-off analysis. |

Rules of thumb:

- Need typed rows of matching values? Use the query builder.
- Building/dropping tables, or running a `CREATE TABLE AS SELECT`? Use SQL.
- Joining multiple tables, computing rankings, or walking a graph? Use SQL.
- Filtering by one or more indexed columns? Use the query builder - it is
  faster and avoids Arrow-to-C decoding.

Mix freely: create tables with SQL, write rows with `mongreldb_put`, read them
back with `mongreldb_query`, and run analytics with SQL.

## Next steps

- [queries.md](queries.md) - every native index condition in detail
- [transactions.md](transactions.md) - bulk inserts via batch transactions
- [errors.md](errors.md) - handling SQL execution errors
