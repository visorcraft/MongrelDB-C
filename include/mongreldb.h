/*
 * mongreldb.h - Public C99 HTTP client for MongrelDB.
 *
 * MongrelDB-C is a libcurl-based client that talks to a running
 * mongreldb-server daemon's JSON API. It mirrors the surface of the PHP, Go,
 * and other official clients: typed CRUD over the Kit transaction endpoint, a
 * query builder that pushes conditions down to the engine's native indexes,
 * batch transactions with idempotency keys, full SQL access, and schema
 * introspection.
 *
 * Memory model:
 *   - mongreldb_client handles are owned by the caller. Free them with
 *     mongreldb_close() (which also releases all owned strings and the query
 *     buffer).
 *   - Result rows are returned via mongreldb_query(). The row buffers and the
 *     values they point at are valid until the next call on the same client, or
 *     until mongreldb_close(). Copy anything you need to keep.
 *   - Error strings (mongreldb_last_error()) are valid until the next call on
 *     the same client.
 *
 * Thread safety:
 *   - A mongreldb_client is NOT thread-safe. Use one client per thread, or
 *     serialize access externally. libcurl itself is initialized globally.
 *
 * Authentication:
 *   - Bearer token (--auth-token mode): mongreldb_connect_with_token().
 *   - HTTP Basic (--auth-users mode): mongreldb_connect_with_basic_auth().
 *
 * Licensing: MIT OR Apache-2.0.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */

#ifndef MONGRELDB_H
#define MONGRELDB_H

/* This is the HTTP client header (links libcurl). It must NOT be included
 * together with mongreldb_engine.h (the native engine ABI header): both
 * declare mongreldb_* symbols with incompatible signatures. Use exactly one
 * per translation unit. */
#ifdef MONGRELDB_ENGINE_H
#error "mongreldb.h and mongreldb_engine.h declare conflicting mongreldb_* symbols. \
Include only one per translation unit: mongreldb.h for the HTTP client, \
mongreldb_engine.h for native engine embedding."
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Symbol export macro ────────────────────────────────────────────────── */
/*
 * The shared library is built with hidden symbol visibility
 * (C_VISIBILITY_PRESET hidden in CMakeLists.txt) so only annotated symbols are
 * exported. MONGRELDB_C_API marks the public surface; when building the library
 * (MONGRELDB_C_EXPORTS defined) the symbols use default visibility / dllexport,
 * otherwise they are imported by consumers.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef MONGRELDB_C_EXPORTS
#    define MONGRELDB_C_API __declspec(dllexport)
#  else
#    define MONGRELDB_C_API __declspec(dllimport)
#  endif
#else
#  if defined(__GNUC__) && __GNUC__ >= 4
#    define MONGRELDB_C_API __attribute__((visibility("default")))
#  else
#    define MONGRELDB_C_API
#  endif
#endif

/* ── Version ────────────────────────────────────────────────────────────── */

#define MONGRELDB_C_VERSION_MAJOR 0
#define MONGRELDB_C_VERSION_MINOR 1
#define MONGRELDB_C_VERSION_PATCH 0

/* ── Error codes ────────────────────────────────────────────────────────── */
/*
 * Every function returns one of these. MDB_OK (0) means success; negative
 * values are failures. Use mongreldb_last_error() / mongreldb_last_error_code()
 * for the detail of the most recent failure on the current client.
 *
 * These mirror the HTTP status mapping of the other clients: auth failures
 * (401/403), not found (404), conflict (409), and everything else.
 */
#define MDB_OK                   0
#define MDB_ERR_AUTH            -1   /* HTTP 401 or 403 */
#define MDB_ERR_NOT_FOUND       -2   /* HTTP 404 */
#define MDB_ERR_CONFLICT        -3   /* HTTP 409 (unique/fk/check violation) */
#define MDB_ERR_QUERY           -4   /* HTTP 400 or 5xx, malformed request */
#define MDB_ERR_NETWORK         -5   /* libcurl transport failure */
#define MDB_ERR_JSON            -6   /* malformed JSON from the server */
#define MDB_ERR_NOMEM           -7   /* out of memory */
#define MDB_ERR_INVALID_ARG     -8   /* NULL or otherwise invalid argument */
#define MDB_ERR_TXN_COMMITTED   -9   /* commit/rollback on a spent transaction */

/* ── Default URL ────────────────────────────────────────────────────────── */

#define MONGRELDB_DEFAULT_URL "http://127.0.0.1:8453"

/* ── Types ──────────────────────────────────────────────────────────────── */

/* Opaque client handle. Create with mongreldb_connect* and free with
 * mongreldb_close(). */
typedef struct mongreldb_client mongreldb_client;

/* A single typed value in a query result row. tag identifies the union arm.
 * String values (MDB_VAL_STRING) are NUL-terminated and point at memory owned
 * by the client; they are valid until the next client call. */
typedef enum {
    MDB_VAL_NULL   = 0,
    MDB_VAL_BOOL   = 1,
    MDB_VAL_INT64  = 2,
    MDB_VAL_DOUBLE = 3,
    MDB_VAL_STRING = 4,
} mongreldb_value_tag;

typedef struct {
    mongreldb_value_tag tag;
    union {
        int b;            /* MDB_VAL_BOOL: 0 or 1 */
        int64_t i64;      /* MDB_VAL_INT64 */
        double f64;       /* MDB_VAL_DOUBLE */
        const char *str;  /* MDB_VAL_STRING: NUL-terminated, client-owned */
    } v;
} mongreldb_value;

/* One cell in a result row: a column id paired with its value. */
typedef struct {
    int64_t column_id;
    mongreldb_value value;
} mongreldb_cell;

/* A result row: an array of cells and a count. The cell array is client-owned
 * and valid until the next client call. */
typedef struct {
    const mongreldb_cell *cells;
    size_t count;
} mongreldb_row;

/* Result set returned by mongreldb_query(). Rows are client-owned and valid
 * until the next client call (or mongreldb_close()). */
typedef struct {
    const mongreldb_row *rows;
    size_t count;
    int truncated;        /* non-zero if the result hit the query limit */
} mongreldb_result;

/* Column definition passed to mongreldb_create_table(). Column ids are stable
 * on-wire identifiers used everywhere else (cells, conditions, projection). */
typedef struct {
    int64_t id;           /* stable numeric column id */
    const char *name;     /* column name */
    const char *ty;       /* type: "int64", "varchar", "float64", "bool", ... */
    int primary_key;      /* non-zero if this is the PK column */
    int nullable;         /* non-zero to allow NULLs */
} mongreldb_column;

/* A staged operation in a transaction. type selects the arm; the fields used
 * depend on the type:
 *   MDB_OP_PUT / MDB_OP_UPSERT: cells + cell_count (and update_cells for upsert)
 *   MDB_OP_DELETE: row_id
 *   MDB_OP_DELETE_BY_PK: pk_value (a single value) */
typedef enum {
    MDB_OP_PUT        = 0,
    MDB_OP_UPSERT     = 1,
    MDB_OP_DELETE     = 2,
    MDB_OP_DELETE_BY_PK = 3,
} mongreldb_op_type;

/* A single cell supplied to a write: column id + value. */
typedef struct {
    int64_t column_id;
    mongreldb_value value;
} mongreldb_input_cell;

typedef struct {
    mongreldb_op_type type;
    const char *table;
    /* PUT / UPSERT inputs (ignored for DELETE): */
    const mongreldb_input_cell *cells;
    size_t cell_count;
    /* UPSERT only: values applied on a PK conflict. NULL update_cells means
     * DO NOTHING on conflict. */
    const mongreldb_input_cell *update_cells;
    size_t update_cell_count;
    /* DELETE by row id: */
    int64_t row_id;
    /* DELETE by primary key: a single value. */
    mongreldb_value pk_value;
} mongreldb_op;

/* Condition kind for the query builder. Each maps to a native engine index.
 * Use the matching fields of mongreldb_condition. */
typedef enum {
    MDB_COND_PK             = 0,   /* exact primary-key match (bytes/value) */
    MDB_COND_BITMAP_EQ      = 1,   /* equality on a bitmap-indexed column */
    MDB_COND_RANGE          = 2,   /* numeric range (lo/hi, inclusive) */
    MDB_COND_FM_CONTAINS    = 3,   /* full-text substring (FM-index) */
    MDB_COND_IS_NULL        = 4,   /* null check */
    MDB_COND_IS_NOT_NULL    = 5,   /* non-null check */
} mongreldb_condition_kind;

typedef struct {
    mongreldb_condition_kind kind;
    int64_t column_id;        /* the numeric column id to filter on */
    /* Numeric range bounds (MDB_COND_RANGE). Leave lo/hi unset for an open
     * end by setting lo_set/hi_set to 0. */
    double lo;
    double hi;
    int lo_set;
    int hi_set;
    int lo_inclusive;         /* default inclusive (1) */
    int hi_inclusive;
    /* PK match, bitmap_eq value, or fm_contains pattern as a string. */
    const char *str_value;
    /* Primary-key value as an integer (used by MDB_COND_PK when str_value is
     * NULL). */
    int64_t int_value;
    int int_set;
} mongreldb_condition;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/* mongreldb_connect creates a client for the daemon at url. Pass NULL or "" to
 * use MONGRELDB_DEFAULT_URL. Returns NULL on allocation failure. */
MONGRELDB_C_API mongreldb_client *mongreldb_connect(const char *url);

/* mongreldb_connect_with_token connects with a Bearer token
 * (--auth-token mode). */
MONGRELDB_C_API mongreldb_client *mongreldb_connect_with_token(const char *url, const char *token);

/* mongreldb_connect_with_basic_auth connects with HTTP Basic credentials
 * (--auth-users mode). */
MONGRELDB_C_API mongreldb_client *mongreldb_connect_with_basic_auth(const char *url,
                                                    const char *username,
                                                    const char *password);

/* mongreldb_set_timeout sets the per-request timeout in seconds (default 30). */
MONGRELDB_C_API void mongreldb_set_timeout(mongreldb_client *c, long timeout_seconds);

/* mongreldb_close releases the client and all owned memory. NULL-safe. */
MONGRELDB_C_API void mongreldb_close(mongreldb_client *c);

/* mongreldb_last_error returns a human-readable message for the most recent
 * failure on c. Valid until the next call on c. Returns "" if no error. */
MONGRELDB_C_API const char *mongreldb_last_error(const mongreldb_client *c);

/* mongreldb_last_error_code returns the error code for the most recent
 * failure, or MDB_OK. */
MONGRELDB_C_API int mongreldb_last_error_code(const mongreldb_client *c);

/* ── Health & tables ────────────────────────────────────────────────────── */

/* mongreldb_health reports whether the daemon is reachable and healthy.
 * Returns MDB_OK on success, a negative error code otherwise. */
MONGRELDB_C_API int mongreldb_health(mongreldb_client *c);

/* mongreldb_table_names lists all table names. out_names receives a
 * NUL-terminated array of string pointers (client-owned, valid until the next
 * call); out_count receives the count. Returns MDB_OK or a negative code. */
MONGRELDB_C_API int mongreldb_table_names(mongreldb_client *c,
                          const char ***out_names, size_t *out_count);

/* mongreldb_create_table creates a table. Returns the assigned table id in
 * *out_table_id, or MDB_OK with *out_table_id == 0 if the server omitted it. */
MONGRELDB_C_API int mongreldb_create_table(mongreldb_client *c,
                           const char *name,
                           const mongreldb_column *columns, size_t column_count,
                           int64_t *out_table_id);

/* mongreldb_drop_table drops a table by name. */
MONGRELDB_C_API int mongreldb_drop_table(mongreldb_client *c, const char *name);

/* mongreldb_count returns the row count for a table in *out_count. */
MONGRELDB_C_API int mongreldb_count(mongreldb_client *c, const char *table, int64_t *out_count);

/* ── CRUD (single-op transactions) ─────────────────────────────────────── */

/* mongreldb_put inserts a row. idempotency_key (or NULL) makes the commit safe
 * to retry. Returns MDB_OK or a negative code. */
MONGRELDB_C_API int mongreldb_put(mongreldb_client *c,
                  const char *table,
                  const mongreldb_input_cell *cells, size_t cell_count,
                  const char *idempotency_key);

/* mongreldb_upsert inserts a row, or updates it on a primary-key conflict.
 * update_cells (or NULL) supplies the values written on conflict. */
MONGRELDB_C_API int mongreldb_upsert(mongreldb_client *c,
                     const char *table,
                     const mongreldb_input_cell *cells, size_t cell_count,
                     const mongreldb_input_cell *update_cells, size_t update_cell_count,
                     const char *idempotency_key);

/* mongreldb_delete removes a row by its internal row id. */
MONGRELDB_C_API int mongreldb_delete(mongreldb_client *c, const char *table, int64_t row_id);

/* mongreldb_delete_by_pk removes a row by its primary-key value. */
MONGRELDB_C_API int mongreldb_delete_by_pk(mongreldb_client *c, const char *table,
                           const mongreldb_value *pk);

/* ── Batch transactions ────────────────────────────────────────────────── */

/* mongreldb_commit sends a batch of operations atomically in a single
 * /kit/txn request. The engine enforces unique, foreign-key, and check
 * constraints at commit time; any violation rolls back the entire batch.
 * idempotency_key (or NULL) makes the commit safe to retry. */
MONGRELDB_C_API int mongreldb_commit(mongreldb_client *c,
                     const mongreldb_op *ops, size_t op_count,
                     const char *idempotency_key);

/* ── Query ─────────────────────────────────────────────────────────────── */

/* mongreldb_query runs a native query against table. conditions (or NULL) are
 * AND-ed; projection (or NULL) restricts returned column ids; limit (or 0)
 * caps the result count. The returned result is client-owned and valid until
 * the next client call. Returns MDB_OK or a negative code. */
MONGRELDB_C_API int mongreldb_query(mongreldb_client *c,
                    const char *table,
                    const mongreldb_condition *conditions, size_t condition_count,
                    const int64_t *projection, size_t projection_count,
                    int64_t limit,
                    mongreldb_result *out_result);

/* mongreldb_result_free is a no-op retained for symmetry; result memory is
 * owned by the client and reused on the next call. Safe to call. */
MONGRELDB_C_API void mongreldb_result_free(mongreldb_result *result);

/* ── SQL ───────────────────────────────────────────────────────────────── */

/* mongreldb_sql executes a SQL statement via the /sql endpoint. For DDL/DML
 * the daemon replies with a non-JSON status body and this returns MDB_OK. For
 * SELECT the daemon typically streams Arrow IPC bytes; the raw (possibly
 * non-JSON) body is returned in *out_body (client-owned, NUL-terminated, valid
 * until the next call) when out_body is non-NULL. */
MONGRELDB_C_API int mongreldb_sql(mongreldb_client *c, const char *sql, const char **out_body);

/* ── Schema ────────────────────────────────────────────────────────────── */

/* mongreldb_schema returns the full schema catalog as the raw JSON body
 * (client-owned, NUL-terminated, valid until the next call). */
MONGRELDB_C_API int mongreldb_schema(mongreldb_client *c, const char **out_body);

/* mongreldb_schema_for returns the descriptor for a single table as the raw
 * JSON body (client-owned, NUL-terminated, valid until the next call). */
MONGRELDB_C_API int mongreldb_schema_for(mongreldb_client *c, const char *table,
                         const char **out_body);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MONGRELDB_H */
