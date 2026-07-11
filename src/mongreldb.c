/* mongreldb.c - libcurl-based C99 HTTP client for MongrelDB.
 *
 * Talks to a running mongreldb-server daemon's JSON API. Includes a minimal,
 * dependency-free JSON parser sufficient to pull out the fields the client
 * cares about (table_id, count, results, rows, truncated, error envelopes).
 *
 * Licensing: MIT OR Apache-2.0.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */

#include "mongreldb.h"

#include <curl/curl.h>

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Tiny dynamic string ────────────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sbuf;

static void sbuf_init(sbuf *s) {
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

static void sbuf_free(sbuf *s) {
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

static int sbuf_reserve(sbuf *s, size_t extra) {
    /* Guard against size_t overflow: len + extra + 1 must not wrap. */
    if (extra > SIZE_MAX - 1 - s->len) {
        return -1;
    }
    size_t need = s->len + extra + 1;
    if (need <= s->cap) {
        return 0;
    }
    size_t ncap = s->cap ? s->cap : 64;
    while (need > ncap) {
        if (ncap > SIZE_MAX / 2) {
            /* Doubling would overflow; clamp to the exact need. */
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    char *nd = (char *)realloc(s->data, ncap);
    if (!nd) {
        return -1;
    }
    s->data = nd;
    s->cap = ncap;
    return 0;
}

static int sbuf_append(sbuf *s, const char *p, size_t n) {
    if (sbuf_reserve(s, n) != 0) {
        return -1;
    }
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
    return 0;
}

static int sbuf_append_str(sbuf *s, const char *str) {
    return sbuf_append(s, str, strlen(str));
}

static int sbuf_append_char(sbuf *s, char ch) {
    return sbuf_append(s, &ch, 1);
}

/* ── Client state ──────────────────────────────────────────────────────── */

struct mongreldb_client {
    char *base_url;        /* no trailing slash */
    char *token;           /* bearer token, or NULL */
    char *username;        /* basic auth, or NULL */
    char *password;        /* basic auth, or NULL */
    long timeout_seconds;

    /* Reusable buffers; the public contract is that returned pointers are
     * valid until the next call, so we keep them on the client. */
    sbuf recv;             /* raw response body */
    sbuf send;             /* request body we build */
    sbuf error_msg;        /* last error string */

    /* Scratch space for table_names output (array of char* + the storage). */
    char **names_arr;
    size_t names_cap;
    sbuf names_blob;

    /* Scratch space for query results (decoded rows/cells/values). */
    mongreldb_row *rows_arr;
    size_t rows_cap;
    mongreldb_cell *cells_arr;
    size_t cells_cap;
    sbuf values_blob;              /* backing strings for the current result */
    /* Per-cell string offsets into values_blob. SIZE_MAX means "not a string".
     * Used to rebaseline v.str pointers after values_blob stops reallocing. */
    size_t *cell_str_offs;
    size_t cell_str_offs_cap;
    /* Per-row cell start offsets. We store offsets (not pointers) while
     * parsing because cells_arr and values_blob may realloc between rows,
     * which would invalidate any raw pointers. Pointers are resolved from
     * these offsets once all rows are decoded. */
    size_t *row_cell_starts;
    size_t row_cell_starts_cap;

    int last_code;
};

/* ── Error helpers ─────────────────────────────────────────────────────── */

static void set_error(mongreldb_client *c, int code, const char *msg) {
    c->last_code = code;
    sbuf_free(&c->error_msg);
    if (msg) {
        sbuf_append_str(&c->error_msg, msg);
    }
}

static void set_error_fmt(mongreldb_client *c, int code, const char *fmt, ...) {
    c->last_code = code;
    sbuf_free(&c->error_msg);
    va_list ap;
    va_start(ap, fmt);
    /* Build into a small stack buffer; error messages are short. */
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sbuf_append_str(&c->error_msg, buf);
}

/* dup_str returns a malloc'd copy of s (NULL-safe). */
static char *dup_str(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (out) {
        memcpy(out, s, n + 1);
    }
    return out;
}

/* ── JSON serialization helpers ────────────────────────────────────────── */

static void json_escape(sbuf *out, const char *s) {
    sbuf_append_char(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  sbuf_append_str(out, "\\\""); break;
            case '\\': sbuf_append_str(out, "\\\\"); break;
            case '\b': sbuf_append_str(out, "\\b");  break;
            case '\f': sbuf_append_str(out, "\\f");  break;
            case '\n': sbuf_append_str(out, "\\n");  break;
            case '\r': sbuf_append_str(out, "\\r");  break;
            case '\t': sbuf_append_str(out, "\\t");  break;
            default:
                if (*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    sbuf_append_str(out, esc);
                } else {
                    sbuf_append_char(out, (char)*p);
                }
                break;
        }
    }
    sbuf_append_char(out, '"');
}

static void json_serialize_value(sbuf *out, const mongreldb_value *v) {
    switch (v->tag) {
        case MDB_VAL_NULL:
            sbuf_append_str(out, "null");
            break;
        case MDB_VAL_BOOL:
            sbuf_append_str(out, v->v.b ? "true" : "false");
            break;
        case MDB_VAL_INT64: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)v->v.i64);
            sbuf_append_str(out, buf);
            break;
        }
        case MDB_VAL_DOUBLE: {
            /* NaN and Infinity have no valid JSON representation; emit null
             * so we never produce invalid JSON that the daemon would reject. */
            if (v->v.f64 != v->v.f64 || v->v.f64 == HUGE_VAL ||
                v->v.f64 == -HUGE_VAL) {
                sbuf_append_str(out, "null");
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.17g", v->v.f64);
                sbuf_append_str(out, buf);
            }
            break;
        }
        case MDB_VAL_STRING:
            json_escape(out, v->v.str ? v->v.str : "");
            break;
        default:
            sbuf_append_str(out, "null");
            break;
    }
}

/* Build the flat cells array the server expects: [col_id, value, ...]. */
static void json_serialize_cells(sbuf *out,
                                 const mongreldb_input_cell *cells, size_t n) {
    sbuf_append_char(out, '[');
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            sbuf_append_char(out, ',');
        }
        char ibuf[32];
        snprintf(ibuf, sizeof(ibuf), "%lld", (long long)cells[i].column_id);
        sbuf_append_str(out, ibuf);
        sbuf_append_char(out, ',');
        json_serialize_value(out, &cells[i].value);
    }
    sbuf_append_char(out, ']');
}

/* Build the JSON object for a single column as sent in /kit/create_table.
 * Extracted from mongreldb_create_table so the wire shape can be unit-tested
 * without a live daemon. Optional fields (enum_variants, default_value) are
 * omitted when their pointer is NULL or, for enum_variants, when the length
 * is zero. */
static void json_serialize_column(sbuf *out, const mongreldb_column *col) {
    sbuf_append_char(out, '{');
    sbuf_append_str(out, "\"id\":");
    char ibuf[32];
    snprintf(ibuf, sizeof(ibuf), "%lld", (long long)col->id);
    sbuf_append_str(out, ibuf);
    sbuf_append_str(out, ",\"name\":");
    json_escape(out, col->name ? col->name : "");
    sbuf_append_str(out, ",\"ty\":");
    json_escape(out, col->ty ? col->ty : "");
    sbuf_append_str(out, ",\"primary_key\":");
    sbuf_append_str(out, col->primary_key ? "true" : "false");
    sbuf_append_str(out, ",\"nullable\":");
    sbuf_append_str(out, col->nullable ? "true" : "false");
    if (col->enum_variants && col->enum_variants_len > 0) {
        sbuf_append_str(out, ",\"enum_variants\":[");
        for (size_t k = 0; k < col->enum_variants_len; k++) {
            if (k > 0) {
                sbuf_append_char(out, ',');
            }
            json_escape(out, col->enum_variants[k] ? col->enum_variants[k] : "");
        }
        sbuf_append_char(out, ']');
    }
    if (col->default_expr) {
        sbuf_append_str(out, ",\"default_expr\":");
        json_escape(out, col->default_expr);
    } else if (col->default_value_json) {
        sbuf_append_str(out, ",\"default_value\":");
        sbuf_append_str(out, col->default_value_json);
    } else if (col->default_value) {
        sbuf_append_str(out, ",\"default_value\":");
        json_escape(out, col->default_value);
    }
    sbuf_append_char(out, '}');
}

/* ── Minimal JSON parser ───────────────────────────────────────────────── */
/*
 * A tiny recursive-descent parser over the response body. It builds no tree;
 * instead we walk it twice - first to find scalar fields we care about
 * (table_id, count, truncated, error.code/error.message/op_index), and then to
 * extract the rows/results arrays into the client's scratch buffers.
 */

typedef struct {
    const char *p;     /* current cursor */
    const char *end;   /* one past the last byte */
    int ok;            /* 0 on parse error */
} json_parser;

static void jp_skip_ws(json_parser *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static int jp_peek(json_parser *j) {
    jp_skip_ws(j);
    return j->p < j->end ? (unsigned char)*j->p : -1;
}

/* jp_value skips over one JSON value at j->p, advancing the cursor. */
static void jp_value(json_parser *j);

static void jp_string(json_parser *j) {
    /* Assumes *p == '"' */
    j->p++;
    while (j->p < j->end) {
        char c = *j->p++;
        if (c == '\\') {
            if (j->p < j->end) {
                j->p++;
            }
        } else if (c == '"') {
            return;
        }
    }
    j->ok = 0;
}

static void jp_number(json_parser *j) {
    while (j->p < j->end) {
        char c = *j->p;
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
            c == 'e' || c == 'E') {
            j->p++;
        } else {
            return;
        }
    }
}

static void jp_literal(json_parser *j) {
    while (j->p < j->end) {
        char c = *j->p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            j->p++;
        } else {
            return;
        }
    }
}

static void jp_array(json_parser *j) {
    /* assumes *p == '[' */
    j->p++;
    jp_skip_ws(j);
    if (jp_peek(j) == ']') {
        j->p++;
        return;
    }
    for (;;) {
        jp_value(j);
        if (!j->ok) {
            return;
        }
        int ch = jp_peek(j);
        if (ch == ',') {
            j->p++;
            continue;
        }
        if (ch == ']') {
            j->p++;
            return;
        }
        j->ok = 0;
        return;
    }
}

static void jp_object(json_parser *j) {
    /* assumes *p == '{' */
    j->p++;
    jp_skip_ws(j);
    if (jp_peek(j) == '}') {
        j->p++;
        return;
    }
    for (;;) {
        jp_skip_ws(j);
        if (jp_peek(j) != '"') {
            j->ok = 0;
            return;
        }
        jp_string(j);
        if (!j->ok) {
            return;
        }
        if (jp_peek(j) != ':') {
            j->ok = 0;
            return;
        }
        j->p++; /* skip ':' */
        jp_value(j);
        if (!j->ok) {
            return;
        }
        int ch = jp_peek(j);
        if (ch == ',') {
            j->p++;
            continue;
        }
        if (ch == '}') {
            j->p++;
            return;
        }
        j->ok = 0;
        return;
    }
}

static void jp_value(json_parser *j) {
    int ch = jp_peek(j);
    switch (ch) {
        case '"': jp_string(j); return;
        case '{': jp_object(j); return;
        case '[': jp_array(j); return;
        case 't': case 'f': case 'n': jp_literal(j); return;
        default:
            if (ch == '-' || (ch >= '0' && ch <= '9')) {
                jp_number(j);
                return;
            }
            j->ok = 0;
            return;
    }
}

/* jp_parse_value initializes a parser positioned at the first value, without
 * consuming it. The caller walks the structure via jp_peek / jp_value. */
static json_parser jp_parse_value(const char *src, size_t len) {
    json_parser j;
    j.p = src;
    j.end = src + len;
    j.ok = 1;
    return j;
}

/* Extract a string field into dst (malloc'd). Returns 1 if found. */
static int json_get_string(const char *src, size_t len,
                           const char *key, char **dst) {
    *dst = NULL;
    json_parser j = jp_parse_value(src, len);
    if (!j.ok || jp_peek(&j) != '{') {
        return 0;
    }
    j.p++; /* skip '{' */
    jp_skip_ws(&j);
    if (jp_peek(&j) == '}') {
        return 0;
    }
    for (;;) {
        jp_skip_ws(&j);
        if (jp_peek(&j) != '"') {
            return 0;
        }
        const char *kstart = j.p + 1;
        jp_string(&j);
        if (!j.ok) {
            return 0;
        }
        size_t klen = (size_t)(j.p - 1 - kstart);
        if (jp_peek(&j) != ':') {
            return 0;
        }
        j.p++;
        int ch = jp_peek(&j);
        if (klen == strlen(key) && memcmp(kstart, key, klen) == 0 && ch == '"') {
            const char *vstart = j.p + 1;
            jp_string(&j);
            if (!j.ok) {
                return 0;
            }
            size_t vlen = (size_t)(j.p - 1 - vstart);
            /* Unescape into dst. */
            char *out = (char *)malloc(vlen + 1);
            if (!out) {
                return 0;
            }
            size_t o = 0;
            for (size_t i = 0; i < vlen; i++) {
                char c = vstart[i];
                if (c == '\\' && i + 1 < vlen) {
                    char n = vstart[++i];
                    switch (n) {
                        case '"': out[o++] = '"'; break;
                        case '\\': out[o++] = '\\'; break;
                        case '/': out[o++] = '/'; break;
                        case 'b': out[o++] = '\b'; break;
                        case 'f': out[o++] = '\f'; break;
                        case 'n': out[o++] = '\n'; break;
                        case 'r': out[o++] = '\r'; break;
                        case 't': out[o++] = '\t'; break;
                        case 'u':
                            /* Best-effort: keep ASCII, drop others. */
                            if (i + 4 < vlen) {
                                char hex[5] = { vstart[i+1], vstart[i+2],
                                                vstart[i+3], vstart[i+4], 0 };
                                unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                                if (cp < 0x80) {
                                    out[o++] = (char)cp;
                                }
                                i += 4;
                            }
                            break;
                        default: out[o++] = n; break;
                    }
                } else {
                    out[o++] = c;
                }
            }
            out[o] = '\0';
            *dst = out;
            return 1;
        }
        jp_value(&j);
        if (!j.ok) {
            return 0;
        }
        ch = jp_peek(&j);
        if (ch == ',') {
            j.p++;
            continue;
        }
        return ch == '}';
    }
}

/* Extract a number field into out_double (and, if integer, out_int). Returns 1
 * if the field was present and parsed as a number. */
static int json_get_number(const char *src, size_t len,
                           const char *key, double *out_double,
                           int64_t *out_int) {
    if (out_double) {
        *out_double = 0;
    }
    if (out_int) {
        *out_int = 0;
    }
    json_parser j = jp_parse_value(src, len);
    if (!j.ok || jp_peek(&j) != '{') {
        return 0;
    }
    j.p++;
    jp_skip_ws(&j);
    if (jp_peek(&j) == '}') {
        return 0;
    }
    for (;;) {
        jp_skip_ws(&j);
        if (jp_peek(&j) != '"') {
            return 0;
        }
        const char *kstart = j.p + 1;
        jp_string(&j);
        if (!j.ok) {
            return 0;
        }
        size_t klen = (size_t)(j.p - 1 - kstart);
        if (jp_peek(&j) != ':') {
            return 0;
        }
        j.p++;
        int ch = jp_peek(&j);
        if (klen == strlen(key) && memcmp(kstart, key, klen) == 0 &&
            (ch == '-' || (ch >= '0' && ch <= '9'))) {
            const char *nstart = j.p;
            jp_number(&j);
            if (!j.ok) {
                return 0;
            }
            char buf[64];
            size_t nlen = (size_t)(j.p - nstart);
            if (nlen >= sizeof(buf)) {
                nlen = sizeof(buf) - 1;
            }
            memcpy(buf, nstart, nlen);
            buf[nlen] = '\0';
            if (out_double) {
                *out_double = strtod(buf, NULL);
            }
            if (out_int) {
                *out_int = (int64_t)strtoll(buf, NULL, 10);
            }
            return 1;
        }
        jp_value(&j);
        if (!j.ok) {
            return 0;
        }
        ch = jp_peek(&j);
        if (ch == ',') {
            j.p++;
            continue;
        }
        return ch == '}';
    }
}

/* json_get_bool extracts a boolean field. Returns 1 if found. */
static int json_get_bool(const char *src, size_t len,
                         const char *key, int *out_bool) {
    *out_bool = 0;
    json_parser j = jp_parse_value(src, len);
    if (!j.ok || jp_peek(&j) != '{') {
        return 0;
    }
    j.p++;
    jp_skip_ws(&j);
    if (jp_peek(&j) == '}') {
        return 0;
    }
    for (;;) {
        jp_skip_ws(&j);
        if (jp_peek(&j) != '"') {
            return 0;
        }
        const char *kstart = j.p + 1;
        jp_string(&j);
        if (!j.ok) {
            return 0;
        }
        size_t klen = (size_t)(j.p - 1 - kstart);
        if (jp_peek(&j) != ':') {
            return 0;
        }
        j.p++;
        int ch = jp_peek(&j);
        if (klen == strlen(key) && memcmp(kstart, key, klen) == 0 &&
            (ch == 't' || ch == 'f')) {
            const char *lstart = j.p;
            jp_literal(&j);
            if (!j.ok) {
                return 0;
            }
            size_t llen = (size_t)(j.p - lstart);
            *out_bool = (llen == 4 && memcmp(lstart, "true", 4) == 0) ? 1 : 0;
            return 1;
        }
        jp_value(&j);
        if (!j.ok) {
            return 0;
        }
        ch = jp_peek(&j);
        if (ch == ',') {
            j.p++;
            continue;
        }
        return ch == '}';
    }
}

/* ── Result row decoding ───────────────────────────────────────────────── */
/*
 * The server returns rows as an array of objects keyed by column id (a string
 * like "2"). We decode each into the client's scratch rows/cells arrays. String
 * values are stored in the values_blob buffer and referenced by pointer.
 */

/* parse_scalar_into reads one JSON value at j->p into v. Strings are appended
 * to blob; the base offset within blob is written to *str_off_out (may be NULL).
 * NOTE: for strings we set v->v.str to a pointer into blob->data, but that
 * pointer is only valid until the next sbuf_reserve on blob. Callers that keep
 * v across multiple parse_scalar_into calls must resolve v->v.str from the
 * returned offset after all parsing is done (see mongreldb_query). */
static void parse_scalar_into(json_parser *j, mongreldb_value *v, sbuf *blob,
                              size_t *str_off_out) {
    int ch = jp_peek(j);
    v->tag = MDB_VAL_NULL;
    if (str_off_out) {
        *str_off_out = (size_t)-1;
    }
    if (ch == '"') {
        const char *start = j->p + 1;
        jp_string(j);
        if (!j->ok) {
            return;
        }
        size_t len = (size_t)(j->p - 1 - start);
        /* Reserve space in blob and unescape into it. */
        if (sbuf_reserve(blob, len) != 0) {
            j->ok = 0;
            return;
        }
        size_t base = blob->len;
        char *dst = blob->data + base;
        size_t o = 0;
        for (size_t i = 0; i < len; i++) {
            char c = start[i];
            if (c == '\\' && i + 1 < len) {
                char n = start[++i];
                switch (n) {
                    case '"':  dst[o++] = '"';  break;
                    case '\\': dst[o++] = '\\'; break;
                    case '/':  dst[o++] = '/';  break;
                    case 'b':  dst[o++] = '\b'; break;
                    case 'f':  dst[o++] = '\f'; break;
                    case 'n':  dst[o++] = '\n'; break;
                    case 'r':  dst[o++] = '\r'; break;
                    case 't':  dst[o++] = '\t'; break;
                    case 'u':
                        if (i + 4 < len) {
                            char hex[5] = { start[i+1], start[i+2],
                                            start[i+3], start[i+4], 0 };
                            unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                            if (cp < 0x80) {
                                dst[o++] = (char)cp;
                            }
                            i += 4;
                        }
                        break;
                    default: dst[o++] = n; break;
                }
            } else {
                dst[o++] = c;
            }
        }
        dst[o] = '\0';
        blob->len += o + 1;
        v->tag = MDB_VAL_STRING;
        v->v.str = dst;
        if (str_off_out) {
            *str_off_out = base;
        }
    } else if (ch == 't' || ch == 'f') {
        const char *start = j->p;
        jp_literal(j);
        size_t l = (size_t)(j->p - start);
        v->tag = MDB_VAL_BOOL;
        v->v.b = (l == 4 && memcmp(start, "true", 4) == 0) ? 1 : 0;
    } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
        const char *start = j->p;
        jp_number(j);
        size_t l = (size_t)(j->p - start);
        char buf[64];
        if (l >= sizeof(buf)) {
            l = sizeof(buf) - 1;
        }
        memcpy(buf, start, l);
        buf[l] = '\0';
        /* If it looks like an integer (no . or e), use int64. */
        int is_int = 1;
        for (size_t i = 0; i < l; i++) {
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') {
                is_int = 0;
                break;
            }
        }
        if (is_int) {
            v->tag = MDB_VAL_INT64;
            v->v.i64 = (int64_t)strtoll(buf, NULL, 10);
        } else {
            v->tag = MDB_VAL_DOUBLE;
            v->v.f64 = strtod(buf, NULL);
        }
    } else if (ch == 'n') {
        jp_literal(j); /* null */
    } else {
        j->ok = 0;
    }
}

/* ── libcurl plumbing ──────────────────────────────────────────────────── */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    sbuf *out = (sbuf *)userdata;
    size_t n = size * nmemb;
    /* Cap the download: abort once the buffered body would exceed the limit
     * so an oversized response is not buffered fully. Returning 0 signals a
     * write error to libcurl, aborting the transfer. */
    if (out->len > MONGRELDB_MAX_RESPONSE_BYTES ||
        n > (size_t)(MONGRELDB_MAX_RESPONSE_BYTES - out->len)) {
        return 0;
    }
    if (sbuf_append(out, ptr, n) != 0) {
        return 0; /* signal error to curl */
    }
    return n;
}

/* do_request performs one HTTP request. method is "GET"/"POST"/"DELETE".
 * request_body is the JSON to send (NULL for no body). The response body lands
 * in c->recv and is NUL-terminated. Returns MDB_OK or a negative code; on HTTP
 * error, also parses the error envelope into set_error. */
static int do_request(mongreldb_client *c, const char *method,
                      const char *path, const char *request_body) {
    if (!c) {
        return MDB_ERR_INVALID_ARG;
    }
    c->last_code = MDB_OK;
    sbuf_free(&c->error_msg);
    sbuf_free(&c->recv);

    /* Build the full URL into the send buffer (reused for the body elsewhere,
     * but never simultaneously). */
    sbuf url;
    sbuf_init(&url);
    sbuf_append_str(&url, c->base_url);
    if (path[0] != '/') {
        sbuf_append_char(&url, '/');
    }
    sbuf_append_str(&url, path);

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(url.data);
        set_error(c, MDB_ERR_NETWORK, "curl_easy_init failed");
        return MDB_ERR_NETWORK;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (request_body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.data);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &c->recv);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, c->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, c->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    /* Restrict the protocols libcurl will follow to http and https so that a
     * caller-supplied base URL cannot be abused for SSRF via other schemes
     * (e.g. file:///etc/passwd). */
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    /* Cap the response body at 256 MB. The write callback also enforces this
     * for chunked transfers where the size is unknown ahead of time. */
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)MONGRELDB_MAX_RESPONSE_BYTES);

    if (request_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_body));
    }

    /* Auth. */
    if (c->token) {
        sbuf auth;
        sbuf_init(&auth);
        if (sbuf_append_str(&auth, "Authorization: Bearer ") != 0 ||
            sbuf_append_str(&auth, c->token) != 0) {
            free(auth.data);
            free(url.data);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            set_error(c, MDB_ERR_NOMEM, "out of memory building auth header");
            return MDB_ERR_NOMEM;
        }
        curl_easy_setopt(curl, CURLOPT_USERPWD, NULL);
        /* headers was already handed to CURLOPT_HTTPHEADER at line 743;
         * curl_slist_append extends that same list in place, so libcurl
         * sees the Authorization header without needing a second setopt. */
        struct curl_slist *appended = curl_slist_append(headers, auth.data);
        free(auth.data);
        if (!appended) {
            free(url.data);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            set_error(c, MDB_ERR_NOMEM, "out of memory appending auth header");
            return MDB_ERR_NOMEM;
        }
        headers = appended;
    } else if (c->username) {
        sbuf creds;
        sbuf_init(&creds);
        sbuf_append_str(&creds, c->username);
        sbuf_append_char(&creds, ':');
        sbuf_append_str(&creds, c->password ? c->password : "");
        curl_easy_setopt(curl, CURLOPT_USERPWD, creds.data);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        free(creds.data);
    }

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(url.data);

    /* An oversized body aborts the transfer (CURLE_FILESIZE_EXCEEDED from
     * CURLOPT_MAXFILESIZE_LARGE, or CURLE_WRITE_ERROR from the write callback)
     * and is reported as MDB_ERR_QUERY. */
    if (rc == CURLE_FILESIZE_EXCEEDED || rc == CURLE_WRITE_ERROR ||
        c->recv.len > (size_t)MONGRELDB_MAX_RESPONSE_BYTES) {
        set_error_fmt(c, MDB_ERR_QUERY,
                      "response body exceeds %lld bytes",
                      (long long)MONGRELDB_MAX_RESPONSE_BYTES);
        return MDB_ERR_QUERY;
    }

    if (rc != CURLE_OK) {
        set_error_fmt(c, MDB_ERR_NETWORK, "network error: %s", curl_easy_strerror(rc));
        return MDB_ERR_NETWORK;
    }

    /* Ensure NUL-terminated body. */
    if (sbuf_reserve(&c->recv, 1) != 0) {
        set_error(c, MDB_ERR_NOMEM, "out of memory reading response");
        return MDB_ERR_NOMEM;
    }
    if (c->recv.len == 0) {
        c->recv.data[0] = '\0';
    } else {
        c->recv.data[c->recv.len] = '\0';
    }

    if (http_code < 200 || http_code >= 300) {
        /* Decode the error envelope. */
        char *msg = NULL;
        char *code = NULL;
        /* Try nested {"error": {...}} by searching the body for the inner
         * object. We approximate by parsing the top-level and nested shapes. */
        if (json_get_string(c->recv.data, c->recv.len, "message", &msg) && msg) {
            json_get_string(c->recv.data, c->recv.len, "code", &code);
        } else {
            /* Fall back to the raw body. */
            if (c->recv.len > 0) {
                msg = dup_str(c->recv.data);
            }
        }
        int mapped;
        const char *sentinel;
        switch (http_code) {
            case 401: case 403:
                mapped = MDB_ERR_AUTH;
                sentinel = "authentication failed";
                break;
            case 404:
                mapped = MDB_ERR_NOT_FOUND;
                sentinel = "resource not found";
                break;
            case 409:
                mapped = MDB_ERR_CONFLICT;
                sentinel = "constraint violation";
                break;
            default:
                mapped = MDB_ERR_QUERY;
                sentinel = "server error";
                break;
        }
        if (!msg) {
            msg = dup_str(sentinel);
        }
        if (code) {
            set_error_fmt(c, mapped, "%s (%s)", msg, code);
        } else {
            set_error(c, mapped, msg);
        }
        free(msg);
        free(code);
        return mapped;
    }

    return MDB_OK;
}

/* Convenience wrappers. */
static int c_get(mongreldb_client *c, const char *path) {
    return do_request(c, "GET", path, NULL);
}
static int c_post(mongreldb_client *c, const char *path, const char *body) {
    return do_request(c, "POST", path, body);
}
static int c_put(mongreldb_client *c, const char *path, const char *body) {
    return do_request(c, "PUT", path, body);
}
static int c_delete(mongreldb_client *c, const char *path) {
    return do_request(c, "DELETE", path, NULL);
}

/* URL-encode a path segment into the send buffer. */
static void url_path_encode(sbuf *out, const char *seg) {
    for (const unsigned char *p = (const unsigned char *)seg; *p; p++) {
        if (*p == '/' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            sbuf_append_char(out, (char)*p);
        } else {
            char esc[4];
            snprintf(esc, sizeof(esc), "%%%02X", *p);
            sbuf_append_str(out, esc);
        }
    }
}

/* ── Public API: lifecycle ─────────────────────────────────────────────── */

mongreldb_client *mongreldb_connect(const char *url) {
    return mongreldb_connect_with_token(url, NULL);
}

mongreldb_client *mongreldb_connect_with_token(const char *url, const char *token) {
    mongreldb_client *c = (mongreldb_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->timeout_seconds = 30;
    sbuf_init(&c->recv);
    sbuf_init(&c->send);
    sbuf_init(&c->error_msg);
    sbuf_init(&c->names_blob);
    sbuf_init(&c->values_blob);
    sbuf_append_str(&c->error_msg, "");

    const char *u = (url && *url) ? url : MONGRELDB_DEFAULT_URL;
    c->base_url = dup_str(u);
    if (!c->base_url) {
        free(c);
        return NULL;
    }
    /* Trim a trailing slash. */
    size_t blen = strlen(c->base_url);
    while (blen > 0 && c->base_url[blen - 1] == '/') {
        c->base_url[--blen] = '\0';
    }
    if (token && *token) {
        c->token = dup_str(token);
        if (!c->token) {
            mongreldb_close(c);
            return NULL;
        }
    }
    return c;
}

mongreldb_client *mongreldb_connect_with_basic_auth(const char *url,
                                                    const char *username,
                                                    const char *password) {
    mongreldb_client *c = mongreldb_connect_with_token(url, NULL);
    if (!c) {
        return NULL;
    }
    if (username && *username) {
        c->username = dup_str(username);
        c->password = dup_str(password ? password : "");
        if (!c->username || !c->password) {
            mongreldb_close(c);
            return NULL;
        }
    }
    return c;
}

void mongreldb_set_timeout(mongreldb_client *c, long timeout_seconds) {
    if (c) {
        c->timeout_seconds = timeout_seconds > 0 ? timeout_seconds : 30;
    }
}

void mongreldb_close(mongreldb_client *c) {
    if (!c) {
        return;
    }
    free(c->base_url);
    free(c->token);
    free(c->username);
    free(c->password);
    sbuf_free(&c->recv);
    sbuf_free(&c->send);
    sbuf_free(&c->error_msg);
    free(c->names_arr);
    sbuf_free(&c->names_blob);
    free(c->rows_arr);
    free(c->cells_arr);
    sbuf_free(&c->values_blob);
    free(c->cell_str_offs);
    free(c->row_cell_starts);
    free(c);
}

const char *mongreldb_last_error(const mongreldb_client *c) {
    return c ? (c->error_msg.data ? c->error_msg.data : "") : "";
}

int mongreldb_last_error_code(const mongreldb_client *c) {
    return c ? c->last_code : MDB_ERR_INVALID_ARG;
}

/* ── Public API: health & tables ───────────────────────────────────────── */

int mongreldb_health(mongreldb_client *c) {
    if (!c) {
        return MDB_ERR_INVALID_ARG;
    }
    return c_get(c, "/health");
}

int mongreldb_table_names(mongreldb_client *c,
                          const char ***out_names, size_t *out_count) {
    if (!c || !out_names || !out_count) {
        return MDB_ERR_INVALID_ARG;
    }
    *out_names = NULL;
    *out_count = 0;
    int rc = c_get(c, "/tables");
    if (rc != MDB_OK) {
        return rc;
    }
    /* The endpoint returns a bare JSON array of strings. Reset the names
     * scratch and walk the array. */
    sbuf_free(&c->names_blob);
    free(c->names_arr);
    c->names_arr = NULL;
    c->names_cap = 0;

    json_parser j = jp_parse_value(c->recv.data, c->recv.len);
    if (!j.ok || jp_peek(&j) != '[') {
        return MDB_OK; /* empty or not an array: treat as no tables */
    }
    j.p++;
    jp_skip_ws(&j);
    if (jp_peek(&j) == ']') {
        j.p++;
        return MDB_OK;
    }
    for (;;) {
        if (jp_peek(&j) != '"') {
            return MDB_ERR_JSON;
        }
        const char *start = j.p + 1;
        jp_string(&j);
        if (!j.ok) {
            return MDB_ERR_JSON;
        }
        size_t len = (size_t)(j.p - 1 - start);
        /* Append to blob, NUL-terminated, and record the pointer. */
        size_t base = c->names_blob.len;
        if (sbuf_append(&c->names_blob, start, len) != 0) {
            return MDB_ERR_NOMEM;
        }
        if (sbuf_append_char(&c->names_blob, '\0') != 0) {
            return MDB_ERR_NOMEM;
        }
        /* Grow the pointer array. */
        if (c->names_cap == 0) {
            c->names_cap = 8;
            c->names_arr = (char **)malloc(c->names_cap * sizeof(char *));
            if (!c->names_arr) {
                return MDB_ERR_NOMEM;
            }
        } else if (*out_count == c->names_cap) {
            c->names_cap *= 2;
            char **na = (char **)realloc(c->names_arr,
                                         c->names_cap * sizeof(char *));
            if (!na) {
                return MDB_ERR_NOMEM;
            }
            c->names_arr = na;
        }
        /* Store the OFFSET (not the pointer) because later appends to
         * names_blob may realloc and dangle it. Pointers are resolved from
         * offsets after the loop completes. */
        c->names_arr[(*out_count)++] = (char *)base;

        int ch = jp_peek(&j);
        if (ch == ',') {
            j.p++;
            continue;
        }
        if (ch == ']') {
            j.p++;
            break;
        }
        return MDB_ERR_JSON;
    }
    /* NUL-terminate the pointer array by appending a NULL slot. */
    if (*out_count == c->names_cap) {
        c->names_cap = *out_count + 1;
        char **na = (char **)realloc(c->names_arr, c->names_cap * sizeof(char *));
        if (!na) {
            return MDB_ERR_NOMEM;
        }
        c->names_arr = na;
    }
    c->names_arr[*out_count] = NULL;

    /* names_blob is now stable. Resolve the stored offsets to real pointers. */
    for (size_t i = 0; i < *out_count; i++) {
        c->names_arr[i] = c->names_blob.data + (size_t)c->names_arr[i];
    }
    *out_names = (const char **)c->names_arr;
    return MDB_OK;
}

static void json_serialize_create_table(sbuf *body,
                                        const char *name,
                                        const mongreldb_column *columns,
                                        size_t column_count,
                                        const char *constraints_json) {
    sbuf_append_str(body, "{\"name\":");
    json_escape(body, name);
    sbuf_append_str(body, ",\"columns\":[");
    for (size_t i = 0; i < column_count; i++) {
        if (i > 0) {
            sbuf_append_char(body, ',');
        }
        json_serialize_column(body, &columns[i]);
    }
    sbuf_append_char(body, ']');
    if (constraints_json && constraints_json[0]) {
        sbuf_append_str(body, ",\"constraints\":");
        sbuf_append_str(body, constraints_json);
    }
    sbuf_append_char(body, '}');
}

int mongreldb_create_table(mongreldb_client *c,
                           const char *name,
                           const mongreldb_column *columns, size_t column_count,
                           int64_t *out_table_id) {
    return mongreldb_create_table_with_constraints_json(
        c, name, columns, column_count, NULL, out_table_id);
}

static int decode_history_retention(mongreldb_client *c,
                                    mongreldb_history_retention *out) {
    int64_t epochs = 0, earliest = 0;
    if (!json_get_number(c->recv.data, c->recv.len,
                         "history_retention_epochs", NULL, &epochs) ||
        !json_get_number(c->recv.data, c->recv.len,
                         "earliest_retained_epoch", NULL, &earliest)) {
        return MDB_ERR_QUERY;
    }
    if (out) {
        out->history_retention_epochs = (uint64_t)epochs;
        out->earliest_retained_epoch = (uint64_t)earliest;
    }
    return MDB_OK;
}

int mongreldb_history_retention_get(mongreldb_client *c,
                                    mongreldb_history_retention *out) {
    int rc = c_get(c, "/history/retention");
    return rc == MDB_OK ? decode_history_retention(c, out) : rc;
}

int mongreldb_history_retention_set(mongreldb_client *c, uint64_t epochs,
                                    mongreldb_history_retention *out) {
    char body[96];
    snprintf(body, sizeof(body), "{\"history_retention_epochs\":%llu}",
             (unsigned long long)epochs);
    int rc = c_put(c, "/history/retention", body);
    return rc == MDB_OK ? decode_history_retention(c, out) : rc;
}

int mongreldb_create_table_with_constraints_json(
                           mongreldb_client *c,
                           const char *name,
                           const mongreldb_column *columns, size_t column_count,
                           const char *constraints_json,
                           int64_t *out_table_id) {
    if (!c || !name || !columns) {
        return MDB_ERR_INVALID_ARG;
    }
    if (out_table_id) {
        *out_table_id = 0;
    }
    sbuf body;
    sbuf_init(&body);
    json_serialize_create_table(&body, name, columns, column_count,
                                constraints_json);

    int rc = c_post(c, "/kit/create_table", body.data);
    free(body.data);
    if (rc != MDB_OK) {
        return rc;
    }
    int64_t tid = 0;
    json_get_number(c->recv.data, c->recv.len, "table_id", NULL, &tid);
    if (out_table_id) {
        *out_table_id = tid;
    }
    return MDB_OK;
}

int mongreldb_drop_table(mongreldb_client *c, const char *name) {
    if (!c || !name) {
        return MDB_ERR_INVALID_ARG;
    }
    sbuf path;
    sbuf_init(&path);
    sbuf_append_str(&path, "/tables/");
    url_path_encode(&path, name);
    int rc = c_delete(c, path.data);
    free(path.data);
    return rc;
}

int mongreldb_count(mongreldb_client *c, const char *table, int64_t *out_count) {
    if (!c || !table || !out_count) {
        return MDB_ERR_INVALID_ARG;
    }
    *out_count = 0;
    sbuf path;
    sbuf_init(&path);
    sbuf_append_str(&path, "/tables/");
    url_path_encode(&path, table);
    sbuf_append_str(&path, "/count");
    int rc = c_get(c, path.data);
    free(path.data);
    if (rc != MDB_OK) {
        return rc;
    }
    int64_t n = 0;
    json_get_number(c->recv.data, c->recv.len, "count", NULL, &n);
    *out_count = n;
    return MDB_OK;
}

/* ── Public API: CRUD ─────────────────────────────────────────────────── */

static int commit_one(mongreldb_client *c, const mongreldb_op *op,
                      const char *idempotency_key) {
    mongreldb_op single = *op;
    return mongreldb_commit(c, &single, 1, idempotency_key);
}

int mongreldb_put(mongreldb_client *c,
                  const char *table,
                  const mongreldb_input_cell *cells, size_t cell_count,
                  const char *idempotency_key) {
    if (!c || !table || (!cells && cell_count > 0)) {
        return MDB_ERR_INVALID_ARG;
    }
    mongreldb_op op;
    memset(&op, 0, sizeof(op));
    op.type = MDB_OP_PUT;
    op.table = table;
    op.cells = cells;
    op.cell_count = cell_count;
    return commit_one(c, &op, idempotency_key);
}

int mongreldb_upsert(mongreldb_client *c,
                     const char *table,
                     const mongreldb_input_cell *cells, size_t cell_count,
                     const mongreldb_input_cell *update_cells, size_t update_cell_count,
                     const char *idempotency_key) {
    if (!c || !table || (!cells && cell_count > 0)) {
        return MDB_ERR_INVALID_ARG;
    }
    mongreldb_op op;
    memset(&op, 0, sizeof(op));
    op.type = MDB_OP_UPSERT;
    op.table = table;
    op.cells = cells;
    op.cell_count = cell_count;
    op.update_cells = update_cells;
    op.update_cell_count = update_cell_count;
    return commit_one(c, &op, idempotency_key);
}

int mongreldb_delete(mongreldb_client *c, const char *table, int64_t row_id) {
    if (!c || !table) {
        return MDB_ERR_INVALID_ARG;
    }
    mongreldb_op op;
    memset(&op, 0, sizeof(op));
    op.type = MDB_OP_DELETE;
    op.table = table;
    op.row_id = row_id;
    return commit_one(c, &op, NULL);
}

int mongreldb_delete_by_pk(mongreldb_client *c, const char *table,
                           const mongreldb_value *pk) {
    if (!c || !table || !pk) {
        return MDB_ERR_INVALID_ARG;
    }
    mongreldb_op op;
    memset(&op, 0, sizeof(op));
    op.type = MDB_OP_DELETE_BY_PK;
    op.table = table;
    op.pk_value = *pk;
    return commit_one(c, &op, NULL);
}

/* ── Public API: batch commit ──────────────────────────────────────────── */

int mongreldb_commit(mongreldb_client *c,
                     const mongreldb_op *ops, size_t op_count,
                     const char *idempotency_key) {
    if (!c || (!ops && op_count > 0)) {
        return MDB_ERR_INVALID_ARG;
    }
    if (op_count == 0) {
        return MDB_OK;
    }
    sbuf body;
    sbuf_init(&body);
    sbuf_append_str(&body, "{\"ops\":[");
    for (size_t i = 0; i < op_count; i++) {
        const mongreldb_op *op = &ops[i];
        if (i > 0) {
            sbuf_append_char(&body, ',');
        }
        sbuf_append_char(&body, '{');
        switch (op->type) {
            case MDB_OP_PUT:
                sbuf_append_str(&body, "\"put\":{");
                sbuf_append_str(&body, "\"table\":");
                json_escape(&body, op->table ? op->table : "");
                sbuf_append_str(&body, ",\"cells\":");
                json_serialize_cells(&body, op->cells, op->cell_count);
                sbuf_append_str(&body, ",\"returning\":false}");
                break;
            case MDB_OP_UPSERT:
                sbuf_append_str(&body, "\"upsert\":{");
                sbuf_append_str(&body, "\"table\":");
                json_escape(&body, op->table ? op->table : "");
                sbuf_append_str(&body, ",\"cells\":");
                json_serialize_cells(&body, op->cells, op->cell_count);
                if (op->update_cells && op->update_cell_count > 0) {
                    sbuf_append_str(&body, ",\"update_cells\":");
                    json_serialize_cells(&body, op->update_cells, op->update_cell_count);
                }
                sbuf_append_str(&body, ",\"returning\":false}");
                break;
            case MDB_OP_DELETE:
                sbuf_append_str(&body, "\"delete\":{");
                sbuf_append_str(&body, "\"table\":");
                json_escape(&body, op->table ? op->table : "");
                sbuf_append_str(&body, ",\"row_id\":");
                char ibuf[32];
                snprintf(ibuf, sizeof(ibuf), "%lld", (long long)op->row_id);
                sbuf_append_str(&body, ibuf);
                sbuf_append_char(&body, '}');
                break;
            case MDB_OP_DELETE_BY_PK:
                sbuf_append_str(&body, "\"delete_by_pk\":{");
                sbuf_append_str(&body, "\"table\":");
                json_escape(&body, op->table ? op->table : "");
                sbuf_append_str(&body, ",\"pk\":");
                json_serialize_value(&body, &op->pk_value);
                sbuf_append_char(&body, '}');
                break;
            default:
                free(body.data);
                set_error(c, MDB_ERR_INVALID_ARG, "unknown op type");
                return MDB_ERR_INVALID_ARG;
        }
        sbuf_append_char(&body, '}');
    }
    sbuf_append_str(&body, "]");
    if (idempotency_key && *idempotency_key) {
        sbuf_append_str(&body, ",\"idempotency_key\":");
        json_escape(&body, idempotency_key);
    }
    sbuf_append_char(&body, '}');

    int rc = c_post(c, "/kit/txn", body.data);
    free(body.data);
    return rc;
}

/* ── Public API: query ────────────────────────────────────────────────── */

static void serialize_condition(sbuf *body, const mongreldb_condition *cond) {
    sbuf_append_char(body, '{');
    switch (cond->kind) {
        case MDB_COND_PK:
            sbuf_append_str(body, "\"pk\":{");
            sbuf_append_str(body, "\"value\":");
            if (cond->str_value) {
                json_escape(body, cond->str_value);
            } else {
                char ibuf[32];
                snprintf(ibuf, sizeof(ibuf), "%lld", (long long)cond->int_value);
                sbuf_append_str(body, ibuf);
            }
            sbuf_append_char(body, '}');
            break;
        case MDB_COND_BITMAP_EQ:
            sbuf_append_str(body, "\"bitmap_eq\":{");
            sbuf_append_str(body, "\"column_id\":");
            {
                char ibuf[32];
                snprintf(ibuf, sizeof(ibuf), "%lld", (long long)cond->column_id);
                sbuf_append_str(body, ibuf);
            }
            sbuf_append_str(body, ",\"value\":");
            json_escape(body, cond->str_value ? cond->str_value : "");
            sbuf_append_char(body, '}');
            break;
        case MDB_COND_RANGE:
            sbuf_append_str(body, "\"range\":{");
            sbuf_append_str(body, "\"column_id\":");
            {
                char ibuf[32];
                snprintf(ibuf, sizeof(ibuf), "%lld", (long long)cond->column_id);
                sbuf_append_str(body, ibuf);
            }
            if (cond->lo_set) {
                char buf[64];
                snprintf(buf, sizeof(buf), ",\"lo\":%.17g", cond->lo);
                sbuf_append_str(body, buf);
            }
            if (cond->hi_set) {
                char buf[64];
                snprintf(buf, sizeof(buf), ",\"hi\":%.17g", cond->hi);
                sbuf_append_str(body, buf);
            }
            sbuf_append_char(body, '}');
            break;
        case MDB_COND_FM_CONTAINS:
            sbuf_append_str(body, "\"fm_contains\":{");
            sbuf_append_str(body, "\"column_id\":");
            {
                char ibuf[32];
                snprintf(ibuf, sizeof(ibuf), "%lld", (long long)cond->column_id);
                sbuf_append_str(body, ibuf);
            }
            sbuf_append_str(body, ",\"pattern\":");
            json_escape(body, cond->str_value ? cond->str_value : "");
            sbuf_append_char(body, '}');
            break;
        case MDB_COND_IS_NULL:
            sbuf_append_str(body, "\"is_null\":{");
            sbuf_append_str(body, "\"column_id\":");
            {
                char ibuf[32];
                snprintf(ibuf, sizeof(ibuf), "%lld", (long long)cond->column_id);
                sbuf_append_str(body, ibuf);
            }
            sbuf_append_char(body, '}');
            break;
        case MDB_COND_IS_NOT_NULL:
            sbuf_append_str(body, "\"is_not_null\":{");
            sbuf_append_str(body, "\"column_id\":");
            {
                char ibuf[32];
                snprintf(ibuf, sizeof(ibuf), "%lld", (long long)cond->column_id);
                sbuf_append_str(body, ibuf);
            }
            sbuf_append_char(body, '}');
            break;
        default:
            sbuf_append_str(body, "\"pk\":{\"value\":null}");
            break;
    }
    sbuf_append_char(body, '}');
}

int mongreldb_query(mongreldb_client *c,
                    const char *table,
                    const mongreldb_condition *conditions, size_t condition_count,
                    const int64_t *projection, size_t projection_count,
                    int64_t limit,
                    mongreldb_result *out_result) {
    if (!c || !table || !out_result) {
        return MDB_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));

    sbuf body;
    sbuf_init(&body);
    sbuf_append_str(&body, "{\"table\":");
    json_escape(&body, table);
    if (conditions && condition_count > 0) {
        sbuf_append_str(&body, ",\"conditions\":[");
        for (size_t i = 0; i < condition_count; i++) {
            if (i > 0) {
                sbuf_append_char(&body, ',');
            }
            serialize_condition(&body, &conditions[i]);
        }
        sbuf_append_char(&body, ']');
    }
    if (projection && projection_count > 0) {
        sbuf_append_str(&body, ",\"projection\":[");
        for (size_t i = 0; i < projection_count; i++) {
            if (i > 0) {
                sbuf_append_char(&body, ',');
            }
            char ibuf[32];
            snprintf(ibuf, sizeof(ibuf), "%lld", (long long)projection[i]);
            sbuf_append_str(&body, ibuf);
        }
        sbuf_append_char(&body, ']');
    }
    if (limit > 0) {
        char ibuf[32];
        snprintf(ibuf, sizeof(ibuf), "%lld", (long long)limit);
        sbuf_append_str(&body, ",\"limit\":");
        sbuf_append_str(&body, ibuf);
    }
    sbuf_append_char(&body, '}');

    int rc = c_post(c, "/kit/query", body.data);
    free(body.data);
    if (rc != MDB_OK) {
        return rc;
    }

    /* Decode the rows array into the client's scratch buffers. Reset them. */
    free(c->rows_arr);
    free(c->cells_arr);
    free(c->cell_str_offs);
    free(c->row_cell_starts);
    sbuf_free(&c->values_blob);
    c->rows_arr = NULL;
    c->cells_arr = NULL;
    c->cell_str_offs = NULL;
    c->cell_str_offs_cap = 0;
    c->row_cell_starts = NULL;
    c->row_cell_starts_cap = 0;
    c->rows_cap = 0;
    c->cells_cap = 0;

    int trunc = 0;
    json_get_bool(c->recv.data, c->recv.len, "truncated", &trunc);
    out_result->truncated = trunc;

    /* The response is {"rows": [{ "<colid>": <value>, ... }, ...], ...}.
     * Walk the "rows" array and decode each row object into the client's
     * scratch cells buffer (rows reference slices of it). String values are
     * unescaped into values_blob and referenced by pointer. */
    json_parser j = jp_parse_value(c->recv.data, c->recv.len);
    if (!j.ok || jp_peek(&j) != '{') {
        return MDB_OK; /* not an object: no rows */
    }
    j.p++;
    jp_skip_ws(&j);
    if (jp_peek(&j) == '}') {
        return MDB_OK;
    }
    /* Scan top-level keys to locate the "rows" array. */
    const char *rows_start = NULL;
    for (;;) {
        jp_skip_ws(&j);
        if (jp_peek(&j) != '"') {
            return MDB_ERR_JSON;
        }
        const char *kstart = j.p + 1;
        jp_string(&j);
        if (!j.ok) {
            return MDB_ERR_JSON;
        }
        size_t klen = (size_t)(j.p - 1 - kstart);
        if (jp_peek(&j) != ':') {
            return MDB_ERR_JSON;
        }
        j.p++;
        if (klen == 4 && memcmp(kstart, "rows", 4) == 0 && jp_peek(&j) == '[') {
            rows_start = j.p;
            break;
        }
        jp_value(&j);
        if (!j.ok) {
            return MDB_ERR_JSON;
        }
        int ch = jp_peek(&j);
        if (ch == ',') {
            j.p++;
            continue;
        }
        if (ch == '}') {
            break;
        }
        return MDB_ERR_JSON;
    }
    if (!rows_start) {
        return MDB_OK;
    }

    /* Walk the rows array with a fresh cursor pointing at rows_start. */
    json_parser w;
    w.p = rows_start;
    w.end = c->recv.data + c->recv.len;
    w.ok = 1;
    w.p++; /* skip '[' */
    jp_skip_ws(&w);
    if (jp_peek(&w) == ']') {
        return MDB_OK;
    }
    size_t total_cells_global = 0; /* accumulates across all rows for fixup */
    for (;;) {
        if (jp_peek(&w) != '{') {
            return MDB_ERR_JSON;
        }
        w.p++;
        jp_skip_ws(&w);

        /* Each row is {"row_id":"...", "cells":[col_id, value, ...]}.
         * Skip over fields until we find the "cells" array, then decode the
         * flat [col_id, value, ...] pairs. */
        size_t total_cells = 0;
        for (size_t r = 0; r < out_result->count; r++) {
            total_cells += c->rows_arr[r].count;
        }
        size_t row_cell_start = total_cells;
        size_t cells_in_row = 0;
        int decoded_cells = 0;

        if (jp_peek(&w) != '}') {
            for (;;) {
                if (jp_peek(&w) != '"') {
                    return MDB_ERR_JSON;
                }
                const char *kstart = w.p + 1;
                jp_string(&w);
                if (!w.ok) {
                    return MDB_ERR_JSON;
                }
                size_t klen = (size_t)(w.p - 1 - kstart);
                if (jp_peek(&w) != ':') {
                    return MDB_ERR_JSON;
                }
                w.p++;

                int is_cells = (klen == 5 && memcmp(kstart, "cells", 5) == 0 &&
                                jp_peek(&w) == '[');
                if (is_cells) {
                    /* Flat array: [col_id, value, col_id, value, ...]. */
                    w.p++;
                    jp_skip_ws(&w);
                    if (jp_peek(&w) == ']') {
                        w.p++;
                        decoded_cells = 1;
                    } else {
                        for (;;) {
                            /* First element of the pair: the column id (a
                             * number). */
                            int ch0 = jp_peek(&w);
                            if (ch0 != '-' && !(ch0 >= '0' && ch0 <= '9')) {
                                return MDB_ERR_JSON;
                            }
                            const char *nstart = w.p;
                            jp_number(&w);
                            if (!w.ok) {
                                return MDB_ERR_JSON;
                            }
                            char idbuf[32];
                            size_t nlen = (size_t)(w.p - nstart);
                            if (nlen >= sizeof(idbuf)) {
                                nlen = sizeof(idbuf) - 1;
                            }
                            memcpy(idbuf, nstart, nlen);
                            idbuf[nlen] = '\0';
                            int64_t colid = (int64_t)strtoll(idbuf, NULL, 10);

                            /* Optional comma then the value. */
                            if (jp_peek(&w) == ',') {
                                w.p++;
                            }
                            /* Grow the cells buffer for one more cell. */
                            if (total_cells + 1 > c->cells_cap) {
                                size_t ncap = c->cells_cap ? c->cells_cap * 2 : 16;
                                while (total_cells + 1 > ncap) {
                                    ncap *= 2;
                                }
                                mongreldb_cell *nc = (mongreldb_cell *)realloc(
                                    c->cells_arr, ncap * sizeof(mongreldb_cell));
                                if (!nc) {
                                    return MDB_ERR_NOMEM;
                                }
                                c->cells_arr = nc;
                                c->cells_cap = ncap;
                            }
                            mongreldb_cell *cell = &c->cells_arr[total_cells];
                            cell->column_id = colid;
                            cell->value.tag = MDB_VAL_NULL;

                            /* Grow the parallel string-offset scratch so we
                             * can rebaseline v.str pointers after parsing. */
                            if (total_cells + 1 > c->cell_str_offs_cap) {
                                size_t ncap =
                                    c->cell_str_offs_cap ? c->cell_str_offs_cap * 2 : 16;
                                while (total_cells + 1 > ncap) {
                                    ncap *= 2;
                                }
                                size_t *no = (size_t *)realloc(
                                    c->cell_str_offs, ncap * sizeof(size_t));
                                if (!no) {
                                    return MDB_ERR_NOMEM;
                                }
                                c->cell_str_offs = no;
                                c->cell_str_offs_cap = ncap;
                            }
                            parse_scalar_into(&w, &cell->value, &c->values_blob,
                                              &c->cell_str_offs[total_cells]);
                            if (!w.ok) {
                                return MDB_ERR_JSON;
                            }
                            total_cells++;
                            total_cells_global = total_cells;
                            cells_in_row++;

                            int ch = jp_peek(&w);
                            if (ch == ',') {
                                w.p++;
                                continue;
                            }
                            if (ch == ']') {
                                w.p++;
                                break;
                            }
                            return MDB_ERR_JSON;
                        }
                    }
                    decoded_cells = 1;
                    /* If this was the last field, the object closes next. */
                    int ch = jp_peek(&w);
                    if (ch == '}') {
                        w.p++;
                        break;
                    }
                    if (ch == ',') {
                        w.p++;
                        continue;
                    }
                    return MDB_ERR_JSON;
                } else {
                    /* Skip the value of an unknown field (e.g. "row_id"). */
                    jp_value(&w);
                    if (!w.ok) {
                        return MDB_ERR_JSON;
                    }
                    int ch = jp_peek(&w);
                    if (ch == ',') {
                        w.p++;
                        continue;
                    }
                    if (ch == '}') {
                        w.p++;
                        break;
                    }
                    return MDB_ERR_JSON;
                }
            }
        } else {
            w.p++; /* empty row object */
        }
        (void)decoded_cells;

        /* Record the row. We store the cell START OFFSET (not a pointer)
         * because c->cells_arr and c->values_blob may realloc while parsing
         * later rows, which would dangle a raw pointer. The offset is
         * resolved to a pointer after the entire rows loop completes. */
        if (out_result->count == c->rows_cap) {
            size_t ncap = c->rows_cap ? c->rows_cap * 2 : 8;
            mongreldb_row *nr = (mongreldb_row *)realloc(
                c->rows_arr, ncap * sizeof(mongreldb_row));
            if (!nr) {
                return MDB_ERR_NOMEM;
            }
            c->rows_arr = nr;
            c->rows_cap = ncap;
        }
        if (out_result->count == c->row_cell_starts_cap) {
            size_t ncap = c->row_cell_starts_cap ? c->row_cell_starts_cap * 2 : 8;
            while (out_result->count >= ncap) {
                ncap *= 2;
            }
            size_t *ns = (size_t *)realloc(c->row_cell_starts,
                                           ncap * sizeof(size_t));
            if (!ns) {
                return MDB_ERR_NOMEM;
            }
            c->row_cell_starts = ns;
            c->row_cell_starts_cap = ncap;
        }
        c->row_cell_starts[out_result->count] = row_cell_start;
        c->rows_arr[out_result->count].count = cells_in_row;
        out_result->count++;
        out_result->rows = c->rows_arr;

        int ch = jp_peek(&w);
        if (ch == ',') {
            w.p++;
            continue;
        }
        if (ch == ']') {
            break;
        }
        return MDB_ERR_JSON;
    }

    /* All parsing is complete and c->cells_arr / c->values_blob are now
     * stable (no more reallocs). Resolve the stored cell-start offsets to real
     * pointers so the caller sees a valid, non-dangling cells slice per row,
     * and rebaseline each string value's pointer into the now-stable
     * values_blob. Rows with no cells get NULL to avoid forming a pointer
     * from a NULL base (UB). */
    for (size_t r = 0; r < out_result->count; r++) {
        c->rows_arr[r].cells = (c->rows_arr[r].count > 0)
                                   ? c->cells_arr + c->row_cell_starts[r]
                                   : NULL;
    }
    for (size_t i = 0; i < total_cells_global; i++) {
        if (c->cell_str_offs[i] != (size_t)-1 &&
            c->cells_arr[i].value.tag == MDB_VAL_STRING) {
            c->cells_arr[i].value.v.str =
                c->values_blob.data + c->cell_str_offs[i];
        }
    }

    return MDB_OK;
}

void mongreldb_result_free(mongreldb_result *result) {
    (void)result; /* memory is owned by the client */
}

/* ── Public API: SQL & schema ──────────────────────────────────────────── */

int mongreldb_sql(mongreldb_client *c, const char *sql, const char **out_body) {
    if (!c || !sql) {
        return MDB_ERR_INVALID_ARG;
    }
    sbuf body;
    sbuf_init(&body);
    sbuf_append_str(&body, "{\"sql\":");
    json_escape(&body, sql);
    sbuf_append_str(&body, ",\"format\":\"json\"}");
    int rc = c_post(c, "/sql", body.data);
    free(body.data);
    if (rc != MDB_OK) {
        return rc;
    }
    if (out_body) {
        *out_body = c->recv.data ? c->recv.data : "";
    }
    return MDB_OK;
}

int mongreldb_schema(mongreldb_client *c, const char **out_body) {
    if (!c || !out_body) {
        return MDB_ERR_INVALID_ARG;
    }
    int rc = c_get(c, "/kit/schema");
    if (rc != MDB_OK) {
        return rc;
    }
    *out_body = c->recv.data ? c->recv.data : "";
    return MDB_OK;
}

int mongreldb_schema_for(mongreldb_client *c, const char *table,
                         const char **out_body) {
    if (!c || !table || !out_body) {
        return MDB_ERR_INVALID_ARG;
    }
    sbuf path;
    sbuf_init(&path);
    sbuf_append_str(&path, "/kit/schema/");
    url_path_encode(&path, table);
    int rc = c_get(c, path.data);
    free(path.data);
    if (rc != MDB_OK) {
        return rc;
    }
    *out_body = c->recv.data ? c->recv.data : "";
    return MDB_OK;
}
