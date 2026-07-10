// test_wire_shape.c - Offline wire-format conformance tests.
//
// Verifies that the JSON serialization for /kit/create_table columns matches
// the daemon's expected wire shape, without needing a running server.
//
// Licensing: MIT OR Apache-2.0.

#include "mongreldb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Declared static in mongreldb.c; redeclared here so we can test it directly.
// The test binary compiles mongreldb.c directly (see CMakeLists.txt).
static void json_serialize_column(sbuf *out, const mongreldb_column *col);

// Reuse the sbuf type from mongreldb.c
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sbuf;

int main(void) {
    // Test 1: Basic column (no optional fields)
    {
        mongreldb_column col = {0};
        col.id = 1;
        col.name = "id";
        col.ty = "int64";
        col.primary_key = 1;
        col.nullable = 0;

        sbuf out = {0};
        json_serialize_column(&out, &col);
        // The wire shape must include id, name, ty, primary_key, nullable
        // and must NOT include enum_variants or default_value when absent.
        assert(strstr(out.data, "\"id\":1") != NULL);
        assert(strstr(out.data, "\"name\":\"id\"") != NULL);
        assert(strstr(out.data, "\"ty\":\"int64\"") != NULL);
        assert(strstr(out.data, "\"primary_key\":true") != NULL);
        assert(strstr(out.data, "\"nullable\":false") != NULL);
        assert(strstr(out.data, "enum_variants") == NULL);
        assert(strstr(out.data, "default_value") == NULL);
        free(out.data);
        printf("PASS: basic column wire shape\n");
    }

    // Test 2: Column with enum_variants
    {
        const char *variants[] = {"active", "inactive", "pending"};
        mongreldb_column col = {0};
        col.id = 2;
        col.name = "status";
        col.ty = "varchar";
        col.primary_key = 0;
        col.nullable = 0;
        col.enum_variants = variants;
        col.enum_variants_len = 3;

        sbuf out = {0};
        json_serialize_column(&out, &col);
        assert(strstr(out.data, "\"enum_variants\":[\"active\",\"inactive\",\"pending\"]") != NULL);
        assert(strstr(out.data, "default_value") == NULL);
        free(out.data);
        printf("PASS: enum_variants wire shape\n");
    }

    // Test 3: Column with default_value
    {
        mongreldb_column col = {0};
        col.id = 3;
        col.name = "score";
        col.ty = "float64";
        col.primary_key = 0;
        col.nullable = 1;
        col.default_value = "0.0";

        sbuf out = {0};
        json_serialize_column(&out, &col);
        assert(strstr(out.data, "\"default_value\":\"0.0\"") != NULL);
        assert(strstr(out.data, "enum_variants") == NULL);
        free(out.data);
        printf("PASS: default_value wire shape\n");
    }

    printf("All wire-shape tests passed.\n");
    return 0;
}
