# Authentication & Authorization

A `mongreldb-server` daemon runs in one of three modes:

1. **Open** (default) - no auth required.
2. **Bearer token** (`--auth-token <TOKEN>`) - every request must carry an
   `Authorization: Bearer <TOKEN>` header.
3. **HTTP Basic** (`--auth-users`) - every request must carry an
   `Authorization: Basic <base64(user:pass)>` header.

The C client supports all three through the `mongreldb_connect_*` constructors.
This guide shows each mode and how to manage users and roles via SQL when the
server is in Basic mode.

---

## Bearer token mode

Start the daemon with a token:

```sh
mongreldb-server --auth-token s3cret-token
```

Connect with `mongreldb_connect_with_token`. The token is sent as
`Authorization: Bearer ...` on every request.

```c
mongreldb_client *db = mongreldb_connect_with_token(
    "http://127.0.0.1:8453", "s3cret-token");

if (mongreldb_health(db) == MDB_ERR_AUTH) {
    fprintf(stderr, "bad or missing token\n");
    return 1;
}
```

A missing or wrong token surfaces as `MDB_ERR_AUTH` (HTTP 401/403).

### Where the token comes from

Hard-coding secrets in source is bad practice. Read it from the environment:

```c
const char *token = getenv("MONGRELDB_TOKEN");
if (token == NULL || *token == '\0') {
    fprintf(stderr, "MONGRELDB_TOKEN not set\n");
    return 1;
}
mongreldb_client *db = mongreldb_connect_with_token(NULL, token);
```

## Basic auth mode

Start the daemon with a users file or inline users:

```sh
mongreldb-server --auth-users
```

Connect with `mongreldb_connect_with_basic_auth`:

```c
mongreldb_client *db = mongreldb_connect_with_basic_auth(
    "http://127.0.0.1:8453", "admin", "s3cret");
```

The client base64-encodes `username:password` and sets
`Authorization: Basic ...` on every request.

## Token takes precedence

A token takes precedence over basic auth. The two constructors are separate, so
in practice you pick one, but the rule holds if you ever layer them.

## Timeouts

`mongreldb_set_timeout` sets the per-request timeout in seconds (default 30).
This applies to both the connect and transfer phases.

```c
mongreldb_client *db = mongreldb_connect_with_token(url, token);
mongreldb_set_timeout(db, 60); /* 60 seconds */
```

## User and role management via SQL

When the daemon is in Basic auth mode, users and roles live in the catalog and
are managed with SQL. Run these statements through `mongreldb_sql`.

### Create a user

```c
mongreldb_sql(db, "CREATE USER alice WITH PASSWORD 'hunter2'", NULL);
```

### Alter a user

Change a password:

```c
mongreldb_sql(db, "ALTER USER alice WITH PASSWORD 'new-password'", NULL);
```

Grant the admin role:

```c
mongreldb_sql(db, "ALTER USER alice ADMIN", NULL);
```

`ALTER USER ... ADMIN` is how you promote a user to full administrative
privileges (table creation/drop, compaction, user management). Use it
sparingly.

### Drop a user

```c
mongreldb_sql(db, "DROP USER alice", NULL);
```

### Roles and grants

```c
mongreldb_sql(db, "CREATE ROLE analyst", NULL);
mongreldb_sql(db, "GRANT SELECT ON orders TO analyst", NULL);
mongreldb_sql(db, "GRANT analyst TO alice", NULL);
mongreldb_sql(db, "REVOKE SELECT ON orders FROM analyst", NULL);
mongreldb_sql(db, "DROP ROLE analyst", NULL);
```

Exact grant syntax mirrors the server's SQL flavor; consult the server's SQL
reference for the full `GRANT`/`REVOKE` grammar available in your build.

## Common pitfalls

**Auth errors look like other errors without the code.** A 401/403 maps to
`MDB_ERR_AUTH`; a 404 maps to `MDB_ERR_NOT_FOUND`. Always switch on the return
code rather than string-matching `mongreldb_last_error()`.

**Forgetting to set auth in production.** A client built with
`mongreldb_connect(NULL)` and no auth sends no credentials. Against an
auth-enabled daemon, every call fails with `MDB_ERR_AUTH`. Centralize client
construction so the auth constructor is never accidentally dropped.

**One client is one identity.** A `mongreldb_client` carries one set of
credentials. If you serve multiple authenticated users, build a client per user
with that user's token, and do not share it across threads (a client is not
thread-safe).

**Token in version control.** Put secrets in the environment, a secret
manager, or a file outside the repo. Never commit a real token.

## Next steps

- [errors.md](errors.md) - `MDB_ERR_AUTH` and the rest of the error codes
- [quickstart.md](quickstart.md) - the full end-to-end walkthrough
