# Transactions

MongrelDB commits every write through a single atomic transaction endpoint
(`POST /kit/txn`). This guide covers the two ways to use it - a one-shot
single op, and a staged batch - plus idempotency keys for safe retries and
constraint-violation handling.

The engine enforces `UNIQUE`, foreign-key, check, and trigger constraints at
**commit time**. A violation aborts the entire batch: no op in the batch
becomes visible.

---

## Single puts vs. batch transactions

### Single op: `mongreldb_put`

`mongreldb_put` is a convenience wrapper that sends a one-op transaction. Use
it when a write is independent and you do not need atomicity across multiple
rows.

```c
mongreldb_input_cell r[] = {
    {1, {MDB_VAL_INT64,  .v.i64 = 1}},
    {2, {MDB_VAL_STRING, .v.str = "Alice"}},
    {3, {MDB_VAL_DOUBLE, .v.f64 = 99.5}},
};
int rc = mongreldb_put(db, "orders", r, 3, NULL /* no idempotency key */);
if (rc != MDB_OK) {
    fprintf(stderr, "put failed: %s\n", mongreldb_last_error(db));
}
```

`mongreldb_upsert`, `mongreldb_delete`, and `mongreldb_delete_by_pk` are the
same shape: single-op transactions.

### Batch: `mongreldb_commit`

When several writes must succeed or fail together, stage them in a `mongreldb_op`
array and commit once. All ops go to the server in a single HTTP request and
commit atomically.

```c
mongreldb_input_cell a[] = {{1, {MDB_VAL_INT64, .v.i64 = 10}},
                            {2, {MDB_VAL_STRING, .v.str = "Dave"}}};
mongreldb_input_cell b[] = {{1, {MDB_VAL_INT64, .v.i64 = 11}},
                            {2, {MDB_VAL_STRING, .v.str = "Eve"}}};
mongreldb_value pk = {MDB_VAL_INT64, .v.i64 = 2};

mongreldb_op ops[3];
memset(ops, 0, sizeof(ops));
ops[0].type = MDB_OP_PUT; ops[0].table = "orders";
ops[0].cells = a;         ops[0].cell_count = 2;
ops[1].type = MDB_OP_PUT; ops[1].table = "orders";
ops[1].cells = b;         ops[1].cell_count = 2;
ops[2].type = MDB_OP_DELETE_BY_PK;
ops[2].table = "orders";  ops[2].pk_value = pk;

int rc = mongreldb_commit(db, ops, 3, NULL);
```

`MDB_OP_UPSERT` takes an additional `update_cells` / `update_cell_count` pair
applied on a primary-key conflict. A NULL `update_cells` means "do nothing on
conflict".

## Idempotency keys for safe retries

Networks drop requests and daemons crash after committing but before replying.
An idempotency key makes a commit safe to retry: the daemon remembers the key
and replays the **original** result on a duplicate commit, even across
restarts.

Pass the key as the last argument to `mongreldb_commit` (or `mongreldb_put` /
`mongreldb_upsert`):

```c
/* A handler that must not double-charge, even if the client retries or the
 * connection drops after the daemon committed. */
mongreldb_input_cell charge[] = {
    {1, {MDB_VAL_STRING, .v.str = order_id}},
    {2, {MDB_VAL_DOUBLE, .v.f64 = 199.0}},
};
mongreldb_op op;
memset(&op, 0, sizeof(op));
op.type = MDB_OP_PUT; op.table = "charges";
op.cells = charge; op.cell_count = 2;

/* Use a stable, business-meaningful key derived from the request. On a retry
 * with the same key the daemon returns the first commit's result instead of
 * inserting a second row. */
mongreldb_commit(db, &op, 1, "charge-order-123");
```

Rules for keys:

- Any non-empty string works. Prefer content-derived, globally-unique values
  (e.g. `"charge:" + order_id`).
- NULL (or the empty string) disables idempotency - a retry will commit again.
- The key scopes the **entire batch**, not individual ops. Reuse the exact
  same ops and key together when retrying.

## Handling constraint violations

Constraint violations arrive as HTTP 409, mapped to `MDB_ERR_CONFLICT`. The
daemon's error envelope (decoded into the client's last-error message) carries
a structured `code` and an `op_index`:

```json
{"status": "aborted", "error": {"code": "UNIQUE_VIOLATION", "message": "...", "op_index": 0}}
```

Check the category and read the message:

```c
int rc = mongreldb_commit(db, ops, 3, NULL);
if (rc == MDB_ERR_CONFLICT) {
    fprintf(stderr, "constraint violated: %s\n", mongreldb_last_error(db));
    /* The engine already rolled back the whole batch. Nothing to undo. */
} else if (rc == MDB_ERR_AUTH) {
    fprintf(stderr, "not authorized: %s\n", mongreldb_last_error(db));
} else if (rc != MDB_OK) {
    fprintf(stderr, "commit failed: %s\n", mongreldb_last_error(db));
}
```

Structured codes you will commonly see in the message:

| code | Meaning |
|------|---------|
| `UNIQUE_VIOLATION` | A unique/PK constraint rejected the commit |
| `FK_VIOLATION` | A foreign-key reference was missing |
| `CHECK_VIOLATION` | A check constraint or trigger rejected the commit |
| `NOT_FOUND` | A named resource (table, schema) does not exist |

## Rollback

There are two notions of "rollback":

1. **Server-side.** When `commit` fails with `MDB_ERR_CONFLICT`, the engine has
   already discarded the entire batch. Nothing was written; there is no server
   rollback to perform.
2. **Client-side.** Because ops are staged in your own `mongreldb_op` array,
   discarding them is just a matter of not calling `mongreldb_commit`. There
   is no transaction handle to roll back - the batch only exists once you send
   it.

```c
mongreldb_op ops[3];
/* ... stage ops ... */
if (!business_rule_ok()) {
    /* Don't commit. The daemon has seen nothing. */
    return;
}
mongreldb_commit(db, ops, 3, NULL);
```

## Summary

| Goal | Use |
|------|-----|
| One independent write | `mongreldb_put` / `upsert` / `delete` / `delete_by_pk` |
| Several writes that must commit together | `mongreldb_commit` with an ops array |
| Retry safely after a network blip | `commit` with a stable idempotency key |
| Distinguish constraint classes | Check `MDB_ERR_CONFLICT` and read the message |
| Abort before sending | Don't call `commit` - the batch is local |

See [errors.md](errors.md) for the full error code set and [queries.md](queries.md)
for read patterns.
