// test_wire_shape.c - Offline wire-format conformance tests.
//
// Verifies that the JSON serialization for /kit/create_table columns matches
// the daemon's expected wire shape, without needing a running server.
//
// Licensing: MIT OR Apache-2.0.

#include "mongreldb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Compile the file-local serializer into this offline test binary.
#include "../src/mongreldb.c"

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

    // Test 2: static JSON scalar defaults preserve literal types.
    {
        const char *values[] = {"\"text\"", "true", "null", "\"now\""};
        const char *wants[] = {"\"default_value\":\"text\"", "\"default_value\":true",
                               "\"default_value\":null", "\"default_value\":\"now\""};
        for (size_t i = 0; i < 4; i++) {
            mongreldb_column col = {0};
            col.default_value_json = values[i];
            sbuf out = {0};
            json_serialize_column(&out, &col);
            assert(strstr(out.data, wants[i]) != NULL);
            assert(strstr(out.data, "default_expr") == NULL);
            free(out.data);
        }
    }

    // Test 3: integer static JSON scalar default
    {
        mongreldb_column col = {0};
        col.id = 4;
        col.name = "attempts";
        col.ty = "int64";
        col.default_value_json = "3";
        sbuf out = {0};
        json_serialize_column(&out, &col);
        assert(strstr(out.data, "\"default_value\":3") != NULL);
        assert(strstr(out.data, "default_expr") == NULL);
        free(out.data);
    }

    // Test 4: dynamic expression takes precedence over static defaults
    {
        mongreldb_column col = {0};
        col.id = 5;
        col.name = "created_at";
        col.ty = "timestamp_nanos";
        col.default_value = "legacy";
        col.default_value_json = "3";
        col.default_expr = "now";
        sbuf out = {0};
        json_serialize_column(&out, &col);
        assert(strstr(out.data, "\"default_expr\":\"now\"") != NULL);
        assert(strstr(out.data, "default_value") == NULL);
        free(out.data);
    }

    // Test 5: Column with enum_variants
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

    // Test 6: legacy string default_value
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

    // Test 7: complete create-table payload with a table CHECK.
    {
        mongreldb_column col = {0};
        col.id = 1;
        col.name = "score";
        col.ty = "int64";
        const char *checks =
            "{\"checks\":[{\"id\":1,\"name\":\"score_nonneg\",\"expr\":"
            "{\"Ge\":[{\"Col\":1},{\"Lit\":{\"Int64\":0}}]}}]}";
        sbuf out = {0};
        json_serialize_create_table(&out, "scores", &col, 1, checks);
        assert(strstr(out.data, "\"constraints\":{\"checks\":[") != NULL);
        assert(strstr(out.data, "\"name\":\"score_nonneg\"") != NULL);
        free(out.data);
        printf("PASS: CHECK constraints wire shape\n");
    }

    // Test 8: history retention request shape and response decoding.
    {
        char body[128];
        assert(history_retention_set_body(100, body, sizeof(body)) == 0);
        assert(strcmp(body, "{\"history_retention_epochs\":100}") == 0);
        assert(strcmp(history_retention_method(), "PUT") == 0);
        assert(strcmp(history_retention_path(), "/history/retention") == 0);

        const char *resp =
            "{\"history_retention_epochs\":100,\"earliest_retained_epoch\":42}";
        mongreldb_history_retention ret = {0};
        assert(history_retention_decode_json(resp, strlen(resp), &ret) == MDB_OK);
        assert(ret.history_retention_epochs == 100);
        assert(ret.earliest_retained_epoch == 42);
        printf("PASS: history retention wire shape\n");
    }

    // Test 9: typed static defaults and dynamic default_expr in one payload.
    {
        mongreldb_column cols[6] = {0};
        cols[0].id = 1; cols[0].name = "c_draft"; cols[0].ty = "varchar";
        cols[0].default_value_json = "\"draft\"";

        cols[1].id = 2; cols[1].name = "c_num"; cols[1].ty = "int64";
        cols[1].default_value_json = "7";

        cols[2].id = 3; cols[2].name = "c_bool"; cols[2].ty = "bool";
        cols[2].default_value_json = "true";

        cols[3].id = 4; cols[3].name = "c_null"; cols[3].ty = "varchar";
        cols[3].default_value_json = "null";

        cols[4].id = 5; cols[4].name = "c_now_lit"; cols[4].ty = "varchar";
        cols[4].default_value_json = "\"now\"";

        cols[5].id = 6; cols[5].name = "c_now_expr"; cols[5].ty = "timestamp_nanos";
        cols[5].default_expr = "now";

        sbuf out = {0};
        json_serialize_create_table(&out, "defaults_matrix", cols, 6, NULL);
        assert(strstr(out.data, "\"default_value\":\"draft\"") != NULL);
        assert(strstr(out.data, "\"default_value\":7") != NULL);
        assert(strstr(out.data, "\"default_value\":true") != NULL);
        assert(strstr(out.data, "\"default_value\":null") != NULL);
        assert(strstr(out.data, "\"default_value\":\"now\"") != NULL);
        assert(strstr(out.data, "\"default_expr\":\"now\"") != NULL);
        free(out.data);
        printf("PASS: typed default matrix wire shape\n");
    }

    printf("All wire-shape tests passed.\n");
    return 0;
}
