# Error handling

Every function in the C client returns an `int` error code. `MDB_OK` (0) means
success; negative values are failures. This is the complete reference: the error
codes, the HTTP-status mapping, the daemon's error envelope, and recovery
patterns for each category.

---

## The error model

The client uses two complementary mechanisms:

1. **Return codes** - `MDB_OK`, `MDB_ERR_AUTH`, `MDB_ERR_NOT_FOUND`,
   `MDB_ERR_CONFLICT`, `MDB_ERR_QUERY`, `MDB_ERR_NETWORK`, `MDB_ERR_JSON`,
   `MDB_ERR_NOMEM`, `MDB_ERR_INVALID_ARG`, `MDB_ERR_TXN_COMMITTED`. Switch on
   these to branch on the *category* of failure.
2. **`mongreldb_last_error(c)`** - a human-readable message for the most recent
   failure, valid until the next call on the same client. It includes the
   daemon's structured error code when the server supplied one.

Every non-`MDB_OK` return sets the last-error string; `MDB_OK` clears it.

## Error code reference

| Code | Value | Meaning | Typical cause |
|------|-------|---------|---------------|
| `MDB_OK` | 0 | success | - |
| `MDB_ERR_AUTH` | -1 | HTTP 401 or 403 | Missing/bad credentials against an auth-enabled daemon |
| `MDB_ERR_NOT_FOUND` | -2 | HTTP 404 | Missing table, missing schema, dropped resource |
| `MDB_ERR_CONFLICT` | -3 | HTTP 409 | Unique, foreign-key, check, or trigger violation at commit |
| `MDB_ERR_QUERY` | -4 | HTTP 400 or 5xx | Malformed request, server-side failure, everything else |
| `MDB_ERR_NETWORK` | -5 | libcurl error | Connection refused, timeout, DNS failure |
| `MDB_ERR_JSON` | -6 | client-side | Malformed JSON response from the server |
| `MDB_ERR_NOMEM` | -7 | client-side | Out of memory |
| `MDB_ERR_INVALID_ARG` | -8 | client-side | NULL or otherwise invalid argument |
| `MDB_ERR_TXN_COMMITTED` | -9 | client-side | Commit/rollback on a spent transaction (reserved) |

## The daemon's error envelope

When the daemon rejects a request, it returns a JSON envelope decoded into the
last-error message:

```json
{
  "status": "aborted",
  "error": {
    "code": "UNIQUE_VIOLATION",
    "message": "duplicate key in column 1",
    "op_index": 0
  }
}
```

Structured codes you will commonly see in the message:

| code | Meaning |
|------|---------|
| `UNIQUE_VIOLATION` | A unique/PK constraint rejected the commit |
| `FK_VIOLATION` | A foreign-key reference was missing |
| `CHECK_VIOLATION` | A check constraint or trigger rejected the commit |
| `NOT_FOUND` | A named resource (table, schema) does not exist |

## HTTP status -> code mapping

| HTTP status | Code | Notes |
|-------------|------|-------|
| 401, 403 | `MDB_ERR_AUTH` | Bad/missing credentials |
| 404 | `MDB_ERR_NOT_FOUND` | Resource not found |
| 409 | `MDB_ERR_CONFLICT` | Constraint violation at commit |
| 400 | `MDB_ERR_QUERY` | Malformed request / bad query |
| 5xx | `MDB_ERR_QUERY` | Daemon-side failure |
| other non-2xx | `MDB_ERR_QUERY` | Catch-all |
| 2xx | `MDB_OK` | Success |

## Discriminating errors

Switch on the return code:

```c
const char *body = NULL;
int rc = mongreldb_schema_for(db, "missing_table", &body);
switch (rc) {
case MDB_OK:
    break;
case MDB_ERR_NOT_FOUND:
    fprintf(stderr, "table does not exist: %s\n", mongreldb_last_error(db));
    break;
case MDB_ERR_CONFLICT:
    fprintf(stderr, "unexpected conflict on a read: %s\n", mongreldb_last_error(db));
    break;
case MDB_ERR_AUTH:
    fprintf(stderr, "bad credentials: %s\n", mongreldb_last_error(db));
    break;
case MDB_ERR_QUERY:
    fprintf(stderr, "server error or malformed request: %s\n", mongreldb_last_error(db));
    break;
case MDB_ERR_NETWORK:
    fprintf(stderr, "can't reach daemon: %s\n", mongreldb_last_error(db));
    break;
default:
    fprintf(stderr, "error: %s\n", mongreldb_last_error(db));
    break;
}
```

## Recovery patterns

### Auth failure - do not retry blindly

A retry will not fix bad credentials. Surface the error to the caller or
operator.

```c
if (rc == MDB_ERR_AUTH) {
    /* Refresh credentials from your secret store, or fail fast. */
    return rc;
}
```

### Not found - fall back, do not crash

For lookups by primary key, a 404 may be a normal "absent" result (when the
table itself is missing). Treat it accordingly.

```c
if (rc == MDB_ERR_NOT_FOUND) {
    /* table missing - treat as empty */
}
```

Note: a `pk` query against an existing table returns zero rows, not a 404;
`MDB_ERR_NOT_FOUND` here means the table itself is missing.

### Constraint conflict - the engine already rolled back

```c
if (rc == MDB_ERR_CONFLICT) {
    fprintf(stderr, "constraint violated: %s\n", mongreldb_last_error(db));
    /* The engine already discarded the whole batch. Nothing to undo. */
}
```

### Transient failure - retry with an idempotency key

`MDB_ERR_NETWORK` and `MDB_ERR_QUERY` (for 5xx) cover transport and transient
server failures. With an idempotency key, retrying a transaction is safe (see
[transactions.md](transactions.md)).

```c
for (int attempt = 0; attempt < 3; attempt++) {
    rc = mongreldb_commit(db, ops, n, "stable-key");
    if (rc == MDB_OK) break;
    if (rc == MDB_ERR_AUTH || rc == MDB_ERR_CONFLICT) break; /* not transient */
    /* sleep and retry */
}
```

### Invalid argument - a programming bug

`MDB_ERR_INVALID_ARG` means a NULL pointer or invalid argument was passed. Fix
the caller rather than catching it at runtime.

### Network failure - check connectivity

`MDB_ERR_NETWORK` wraps the libcurl error. The message includes the curl error
string. Check whether the daemon is running and reachable on the configured
URL.

## Quick reference

```c
#include <mongreldb.h>

/* Category check: */
if (rc == MDB_ERR_NOT_FOUND) { /* ... */ }
if (rc == MDB_ERR_CONFLICT)  { /* ... */ }
if (rc == MDB_ERR_AUTH)      { /* ... */ }
if (rc == MDB_ERR_QUERY)     { /* ... */ }
if (rc == MDB_ERR_NETWORK)   { /* ... */ }

/* Detail extraction: */
const char *msg = mongreldb_last_error(db);   /* valid until next call */
int code = mongreldb_last_error_code(db);     /* same as rc for the last call */
```

## Next steps

- [transactions.md](transactions.md) - constraint handling and retries in context
- [auth.md](auth.md) - credential management
