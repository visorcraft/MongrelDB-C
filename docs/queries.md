# Queries

The `mongreldb_query` function pushes conditions down to MongrelDB's native
indexes for sub-millisecond lookups - bitmap, learned-range, FM-index full
text, and more. Each condition type maps to one specialized index; conditions
are AND-ed together.

```c
mongreldb_condition cond = {
    .kind = MDB_COND_RANGE, .column_id = 3,
    .lo = 100.0, .lo_set = 1, .hi = 500.0, .hi_set = 1,
};
int64_t proj[] = {1, 2};
mongreldb_result res;
mongreldb_query(db, "orders", &cond, 1, proj, 2, 100, &res);
```

This guide covers every condition type, projection, limits and truncation, and
combining conditions.

---

## The basics

Every query call takes the table, an array of conditions, a projection, a
limit, and an output result:

| Argument | Purpose |
|----------|---------|
| `conditions` (or NULL) | An array of native conditions. All are AND-ed. |
| `projection` (or NULL) | Return only these column ids (NULL means all columns). |
| `limit` (or 0) | Cap the number of rows. |
| `out_result` | Receives the rows and a `truncated` flag. |

The request body the client builds matches the daemon's `/kit/query` shape:

```json
{
  "table": "orders",
  "conditions": [{"range": {"column_id": 3, "lo": 100.0, "hi": 500.0}}],
  "projection": [1, 2],
  "limit": 100
}
```

Result memory is owned by the client and valid until the next call (or
`mongreldb_close()`). Copy anything you need to keep.

## Condition types

Each `mongreldb_condition` has a `kind` and a set of fields. Column references
use the numeric **column id**, never the column name.

### `MDB_COND_PK` - exact primary-key match

The fastest lookup. Supply the primary-key value as `int_value` (for integer
PKs) or `str_value` (for string PKs).

```c
mongreldb_condition cond = {
    .kind = MDB_COND_PK, .int_value = 42, .int_set = 1,
};
mongreldb_query(db, "orders", &cond, 1, NULL, 0, 0, &res);
```

### `MDB_COND_RANGE` - numeric range (learned-range index)

Inclusive bounds. Leave `lo_set` / `hi_set` at 0 for an open end.

```c
mongreldb_condition cond = {
    .kind = MDB_COND_RANGE, .column_id = 3,
    .lo = 100.0, .lo_set = 1,
    .hi = 500.0, .hi_set = 1,
};
mongreldb_query(db, "orders", &cond, 1, NULL, 0, 0, &res);

/* Open-ended: amount >= 100 */
mongreldb_condition open_cond = {
    .kind = MDB_COND_RANGE, .column_id = 3,
    .lo = 100.0, .lo_set = 1,
};
```

### `MDB_COND_BITMAP_EQ` - equality on a bitmap-indexed column

Best for low-cardinality columns (status, category, booleans).

```c
mongreldb_condition cond = {
    .kind = MDB_COND_BITMAP_EQ, .column_id = 2, .str_value = "Alice",
};
mongreldb_query(db, "orders", &cond, 1, NULL, 0, 0, &res);
```

### `MDB_COND_IS_NULL` / `MDB_COND_IS_NOT_NULL` - null checks

```c
mongreldb_condition is_null = {
    .kind = MDB_COND_IS_NULL, .column_id = 3,
};
mongreldb_condition not_null = {
    .kind = MDB_COND_IS_NOT_NULL, .column_id = 3,
};
```

### `MDB_COND_FM_CONTAINS` - full-text substring search (FM-index)

Substring match within a column. The `str_value` becomes the on-wire `pattern`.

```c
mongreldb_condition cond = {
    .kind = MDB_COND_FM_CONTAINS, .column_id = 2,
    .str_value = "database performance",
};
mongreldb_query(db, "documents", &cond, 1, NULL, 0, 10, &res);
```

For vector similarity (`ann`), sparse match, and MinHash similarity, use SQL
or extend the condition kinds - the server supports them on the wire; this
client covers the most common index conditions. See [sql.md](sql.md) for the
ones not yet exposed as native helpers.

## Projection (column selection)

Pass a `projection` array to restrict the columns in each returned row. Pass
NULL for all columns. Projecting to only the columns you need cuts bandwidth
and decode cost.

```c
int64_t proj[] = {1, 2};  /* id and customer only */
mongreldb_query(db, "orders", &cond, 1, proj, 2, 100, &res);
```

Returned cells are decoded into the `mongreldb_value` tagged union. Check
`cell->value.tag` to read the right union arm:

```c
for (size_t i = 0; i < res.count; i++) {
    for (size_t j = 0; j < res.rows[i].count; j++) {
        mongreldb_cell *c = &res.rows[i].cells[j];
        switch (c->value.tag) {
        case MDB_VAL_INT64:  printf("col %lld = %lld\n",
                                    (long long)c->column_id,
                                    (long long)c->value.v.i64); break;
        case MDB_VAL_DOUBLE: printf("col %lld = %g\n",
                                    (long long)c->column_id,
                                    c->value.v.f64); break;
        case MDB_VAL_STRING: printf("col %lld = %s\n",
                                    (long long)c->column_id,
                                    c->value.v.str); break;
        case MDB_VAL_NULL:   printf("col %lld = null\n",
                                    (long long)c->column_id); break;
        default: break;
        }
    }
}
```

## Limit and the truncated flag

A non-zero `limit` caps the result. When the server has more matches than the
limit allows, it returns the first `limit` and sets `truncated` to 1.

```c
mongreldb_result res;
mongreldb_query(db, "orders", &cond, 1, NULL, 0, 100, &res);
if (res.truncated) {
    /* 100 rows came back but more exist on the server. Either raise the
     * limit, page with a range predicate on the PK, or accept the cap. */
}
```

## Multiple AND conditions

Pass an array of conditions. Every condition must match; the server intersects
the index results.

```c
/* Customer is Alice AND amount is between 100 and 500. */
mongreldb_condition conds[2];
conds[0].kind = MDB_COND_BITMAP_EQ; conds[0].column_id = 2;
conds[0].str_value = "Alice";
conds[1].kind = MDB_COND_RANGE; conds[1].column_id = 3;
conds[1].lo = 100.0; conds[1].lo_set = 1;
conds[1].hi = 500.0; conds[1].hi_set = 1;

int64_t proj[] = {1, 3};
mongreldb_query(db, "orders", conds, 2, proj, 2, 50, &res);
```

Because each condition targets a different specialized index, the engine can
pick the most selective one to drive the lookup and intersect the rest.

## Putting it together

A realistic combined lookup - bitmap equality + range + projection + limit +
truncation check:

```c
void top_spenders(mongreldb_client *db, const char *customer) {
    mongreldb_condition conds[2];
    memset(conds, 0, sizeof(conds));
    conds[0].kind = MDB_COND_BITMAP_EQ; conds[0].column_id = 2;
    conds[0].str_value = customer;
    conds[1].kind = MDB_COND_RANGE; conds[1].column_id = 3;
    conds[1].lo = 100.0; conds[1].lo_set = 1;

    int64_t proj[] = {1, 3};
    mongreldb_result res;
    if (mongreldb_query(db, "orders", conds, 2, proj, 2, 50, &res) != MDB_OK) {
        return;
    }
    if (res.truncated) {
        fprintf(stderr, "warning: result capped at 50\n");
    }
    /* ... read res.rows ... */
}
```

For arbitrary predicates, joins, and aggregations that the native indexes do
not cover, use SQL instead - see [sql.md](sql.md).
