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
    // Raw JSON values carry embedding, sparse, set, array, and object values.
    {
        mongreldb_value value = {.tag = MDB_VAL_JSON, .v.json = "[1.0,-2.0]"};
        sbuf out = {0};
        json_serialize_value(&out, &value);
        assert(strcmp(out.data, "[1.0,-2.0]") == 0);
        free(out.data);

        const char *raw = "[[7,0.5]]";
        json_parser parser = {.p = raw, .end = raw + strlen(raw), .ok = 1};
        sbuf blob = {0};
        mongreldb_value decoded = {0};
        parse_scalar_into(&parser, &decoded, &blob, NULL);
        assert(parser.ok);
        assert(decoded.tag == MDB_VAL_JSON);
        assert(strcmp(decoded.v.json, "[[7,0.5]]") == 0);
        free(blob.data);
    }

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
        json_serialize_create_table_with_indexes(
            &out, "scores", &col, 1, checks, NULL, 0);
        assert(strstr(out.data, "\"constraints\":{\"checks\":[") != NULL);
        assert(strstr(out.data, "\"name\":\"score_nonneg\"") != NULL);
        free(out.data);
        printf("PASS: CHECK constraints wire shape\n");
    }

    // Test 8: history retention request shape and response decoding.
    {
        mongreldb_column columns[2] = {0};
        columns[0].id = 1;
        columns[0].name = "id";
        columns[0].ty = "int64";
        columns[0].primary_key = 1;
        columns[1].id = 2;
        columns[1].name = "embedding";
        columns[1].ty = "embedding(384)";
        columns[1].embedding_source_json =
            "{\"kind\":\"configured_model\",\"provider_id\":\"docs\","
            "\"model_id\":\"model\",\"model_version\":\"1\"}";
        mongreldb_index indexes[] = {
            {.name = "bm", .column_id = 1, .kind = MDB_INDEX_BITMAP},
            {.name = "fm", .column_id = 1, .kind = MDB_INDEX_FM},
            {.name = "ann", .column_id = 2, .kind = MDB_INDEX_ANN,
             .predicate = "embedding IS NOT NULL", .ann_m = 24,
             .ann_ef_construction = 96, .ann_ef_search = 48,
             .ann_quantization = MDB_ANN_QUANTIZATION_DENSE},
            {.name = "range", .column_id = 1, .kind = MDB_INDEX_LEARNED_RANGE,
             .learned_range_epsilon = 8},
            {.name = "minhash", .column_id = 1, .kind = MDB_INDEX_MIN_HASH,
             .minhash_permutations = 64, .minhash_bands = 16},
            {.name = "sparse", .column_id = 1, .kind = MDB_INDEX_SPARSE},
        };
        sbuf out = {0};
        json_serialize_create_table_with_indexes(
            &out, "search_docs", columns, 2, NULL, indexes, 6);
        assert(strstr(out.data, "\"embedding_source\":{\"kind\":\"configured_model\"") != NULL);
        assert(strstr(out.data, "\"kind\":\"bitmap\"") != NULL);
        assert(strstr(out.data, "\"kind\":\"fm_index\"") != NULL);
        assert(strstr(out.data, "\"kind\":\"ann\"") != NULL);
        assert(strstr(out.data, "\"quantization\":\"dense\"") != NULL);
        assert(strstr(out.data, "\"m\":24") != NULL);
        assert(strstr(out.data, "\"predicate\":\"embedding IS NOT NULL\"") != NULL);
        assert(strstr(out.data, "\"kind\":\"learned_range\"") != NULL);
        assert(strstr(out.data, "\"epsilon\":8") != NULL);
        assert(strstr(out.data, "\"kind\":\"minhash\"") != NULL);
        assert(strstr(out.data, "\"permutations\":64") != NULL);
        assert(strstr(out.data, "\"kind\":\"sparse\"") != NULL);
        free(out.data);
        printf("PASS: all index kinds and embedding source wire shape\n");
    }

    // Test 9: history retention request shape and response decoding.
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

    // Test 10: typed static defaults and dynamic default_expr in one payload.
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
        json_serialize_create_table_with_indexes(
            &out, "defaults_matrix", cols, 6, NULL, NULL, 0);
        assert(strstr(out.data, "\"default_value\":\"draft\"") != NULL);
        assert(strstr(out.data, "\"default_value\":7") != NULL);
        assert(strstr(out.data, "\"default_value\":true") != NULL);
        assert(strstr(out.data, "\"default_value\":null") != NULL);
        assert(strstr(out.data, "\"default_value\":\"now\"") != NULL);
        assert(strstr(out.data, "\"default_expr\":\"now\"") != NULL);

        // Type discrimination: numbers, bools, and nulls must NOT be
        // string-quoted.  If the serializer accidentally wrapped a scalar in
        // quotes, the positive assertion above could still match a substring
        // of the wrong type.  These negative assertions close that gap.
        assert(strstr(out.data, "\"default_value\":\"7\"") == NULL);
        assert(strstr(out.data, "\"default_value\":\"true\"") == NULL);
        assert(strstr(out.data, "\"default_value\":\"null\"") == NULL);
        // Conversely, string defaults MUST carry quotes — an unquoted scalar
        // would indicate the string was emitted as a bare identifier.
        assert(strstr(out.data, "\"default_value\":draft") == NULL);
        assert(strstr(out.data, "\"default_value\":now,") == NULL);
        free(out.data);
        printf("PASS: typed default matrix wire shape\n");
    }

    // Test 11: error propagation — non-2xx HTTP status maps to typed error codes.
    //
    // The /history/retention transport functions propagate whatever code
    // do_request returns.  do_request classifies the HTTP status via
    // classify_http_error, so verifying the classification IS verifying the
    // propagation contract: a 503 body must surface as MDB_ERR_QUERY, a 401
    // as MDB_ERR_AUTH, etc.
    {
        assert(classify_http_error(503, "server overloaded") == MDB_ERR_QUERY);
        assert(classify_http_error(500, NULL) == MDB_ERR_QUERY);
        assert(classify_http_error(400, "bad request") == MDB_ERR_QUERY);
        assert(classify_http_error(401, NULL) == MDB_ERR_AUTH);
        assert(classify_http_error(403, NULL) == MDB_ERR_AUTH);
        assert(classify_http_error(404, NULL) == MDB_ERR_NOT_FOUND);
        assert(classify_http_error(409, NULL) == MDB_ERR_CONFLICT);
        // A "not found:" message prefix overrides the default bucket.
        assert(classify_http_error(500, "not found: table gone") ==
               MDB_ERR_NOT_FOUND);
        printf("PASS: HTTP error status propagation\n");
    }

    // Test 12: error envelope decode — the JSON parser extracts message/code
    // from a flat error body the same way do_request does.
    {
        const char *flat =
            "{\"message\":\"server overloaded\",\"code\":\"UNAVAILABLE\"}";
        char *msg = NULL;
        char *code = NULL;
        assert(json_get_string(flat, strlen(flat), "message", &msg));
        assert(msg != NULL);
        assert(strcmp(msg, "server overloaded") == 0);
        assert(json_get_string(flat, strlen(flat), "code", &code));
        assert(code != NULL);
        assert(strcmp(code, "UNAVAILABLE") == 0);
        free(msg);
        free(code);
        printf("PASS: error envelope decode\n");
    }

    // Test 13: swappable ANN algorithms (DiskANN / IVF) and product
    // quantization serialize to the expected JSON wire shape.
    {
        mongreldb_index diskann = {
            .name = "ann_diskann", .column_id = 2, .kind = MDB_INDEX_ANN,
            .ann_quantization = MDB_ANN_QUANTIZATION_DENSE,
            .ann_algorithm = MDB_ANN_ALGORITHM_DISKANN,
            .diskann_r = 128, .diskann_l = 256, .diskann_beam_width = 4,
            .diskann_alpha = 130};
        sbuf out = {0};
        json_serialize_index(&out, &diskann);
        assert(strstr(out.data, "\"algorithm\":\"diskann\"") != NULL);
        assert(strstr(out.data, "\"quantization\":\"dense\"") != NULL);
        assert(strstr(out.data, "\"diskann\":{\"r\":128") != NULL);
        assert(strstr(out.data, "\"l\":256") != NULL);
        assert(strstr(out.data, "\"beam_width\":4") != NULL);
        assert(strstr(out.data, "\"alpha\":130") != NULL);
        free(out.data);

        mongreldb_index ivf = {
            .name = "ann_ivf", .column_id = 2, .kind = MDB_INDEX_ANN,
            .ann_quantization = MDB_ANN_QUANTIZATION_DENSE,
            .ann_algorithm = MDB_ANN_ALGORITHM_IVF,
            .ivf_nlist = 512, .ivf_nprobe = 16};
        out = (sbuf){0};
        json_serialize_index(&out, &ivf);
        assert(strstr(out.data, "\"algorithm\":\"ivf\"") != NULL);
        assert(strstr(out.data, "\"ivf\":{\"nlist\":512,\"nprobe\":16}") != NULL);
        free(out.data);

        mongreldb_index pq = {
            .name = "ann_pq", .column_id = 2, .kind = MDB_INDEX_ANN,
            .ann_quantization = MDB_ANN_QUANTIZATION_PRODUCT,
            .pq_num_subvectors = 32, .pq_bits = 8,
            .pq_training_samples = 10000, .pq_seed = 42,
            .pq_rerank_factor = 3};
        out = (sbuf){0};
        json_serialize_index(&out, &pq);
        assert(strstr(out.data, "\"quantization\":\"product\"") != NULL);
        assert(strstr(out.data, "\"product\":{\"training_samples\":10000") != NULL);
        assert(strstr(out.data, "\"seed\":42") != NULL);
        assert(strstr(out.data, "\"rerank_factor\":3") != NULL);
        /* Default algorithm (HNSW) is omitted to preserve wire shape. */
        assert(strstr(out.data, "\"algorithm\":") == NULL);
        free(out.data);

        printf("PASS: swappable ANN algorithm and product-quantization wire shape\n");
    }

    // Test 12: durable HLC query-status decode (structural, no free-form scrape).
    {
        const char *raw =
            "{"
            "\"query_id\":\"abcdefabcdefabcdefabcdefabcdefab\","
            "\"status\":\"committed\","
            "\"state\":\"completed\","
            "\"server_state\":\"completed\","
            "\"terminal_state\":\"committed\","
            "\"committed\":true,"
            "\"committed_statements\":1,"
            "\"last_commit_epoch\":17,"
            "\"last_commit_epoch_text\":\"17\","
            "\"last_commit_hlc\":{"
            "\"physical_micros\":1700000000000000,"
            "\"logical\":3,"
            "\"node_tiebreaker\":7"
            "},"
            "\"outcome\":{"
            "\"committed\":true,"
            "\"committed_statements\":1,"
            "\"last_commit_epoch\":17,"
            "\"last_commit_hlc\":{"
            "\"physical_micros\":1700000000000000,"
            "\"logical\":3,"
            "\"node_tiebreaker\":7"
            "},"
            "\"serialization\":\"succeeded\","
            "\"serialization_state\":\"succeeded\","
            "\"terminal_state\":\"committed\""
            "},"
            "\"durable\":{"
            "\"committed\":true,"
            "\"committed_statements\":1,"
            "\"last_commit_epoch\":17,"
            "\"last_commit_hlc\":{"
            "\"physical_micros\":1700000000000000,"
            "\"logical\":3,"
            "\"node_tiebreaker\":7"
            "},"
            "\"serialization\":\"succeeded\","
            "\"serialization_state\":\"succeeded\","
            "\"terminal_state\":\"committed\""
            "}"
            "}";
        sbuf blob = {0};
        mongreldb_query_status st = {0};
        assert(query_status_decode_json(raw, strlen(raw), &st, &blob) == MDB_OK);
        assert(st.committed_set && st.committed == 1);
        assert(st.durable_set);
        assert(st.outcome.last_commit_epoch_set &&
               st.outcome.last_commit_epoch == 17);
        const mongreldb_commit_hlc *hlc = mongreldb_query_status_commit_hlc(&st);
        assert(hlc != NULL);
        assert(hlc->physical_micros == 1700000000000000ULL);
        assert(hlc->logical == 3);
        assert(hlc->node_tiebreaker == 7);
        assert(strcmp(mongreldb_query_status_serialization_state(&st),
                      "succeeded") == 0);
        free(blob.data);
        printf("PASS: durable HLC query-status structural decode\n");
    }

    // Test 13: commit_hlc prefers durable over outcome over top-level.
    {
        const char *raw =
            "{"
            "\"last_commit_hlc\":{"
            "\"physical_micros\":1,\"logical\":1,\"node_tiebreaker\":1"
            "},"
            "\"outcome\":{"
            "\"last_commit_hlc\":{"
            "\"physical_micros\":2,\"logical\":2,\"node_tiebreaker\":2"
            "}"
            "},"
            "\"durable\":{"
            "\"last_commit_hlc\":{"
            "\"physical_micros\":3,\"logical\":3,\"node_tiebreaker\":3"
            "}"
            "}"
            "}";
        sbuf blob = {0};
        mongreldb_query_status st = {0};
        assert(query_status_decode_json(raw, strlen(raw), &st, &blob) == MDB_OK);
        const mongreldb_commit_hlc *hlc = mongreldb_query_status_commit_hlc(&st);
        assert(hlc != NULL && hlc->physical_micros == 3);
        free(blob.data);

        const char *raw2 =
            "{"
            "\"last_commit_hlc\":{"
            "\"physical_micros\":1,\"logical\":1,\"node_tiebreaker\":1"
            "},"
            "\"outcome\":{"
            "\"last_commit_hlc\":{"
            "\"physical_micros\":2,\"logical\":2,\"node_tiebreaker\":2"
            "}"
            "}"
            "}";
        blob = (sbuf){0};
        st = (mongreldb_query_status){0};
        assert(query_status_decode_json(raw2, strlen(raw2), &st, &blob) == MDB_OK);
        hlc = mongreldb_query_status_commit_hlc(&st);
        assert(hlc != NULL && hlc->physical_micros == 2);
        free(blob.data);
        printf("PASS: commit_hlc preference order durable > outcome > top\n");
    }

    // Test 14: retrieve_text request body wire shape.
    {
        sbuf body = {0};
        assert(retrieve_text_build_body("docs", 3, "cat sat", 5, &body) == 0);
        assert(strstr(body.data, "\"table\":\"docs\"") != NULL);
        assert(strstr(body.data, "\"embedding_column\":3") != NULL);
        assert(strstr(body.data, "\"text\":\"cat sat\"") != NULL);
        assert(strstr(body.data, "\"k\":5") != NULL);
        free(body.data);

        body = (sbuf){0};
        assert(retrieve_text_build_body("docs", 3, "cat sat", 0, &body) == 0);
        assert(strstr(body.data, "\"k\":") == NULL);
        free(body.data);
        printf("PASS: retrieve_text request body wire shape\n");
    }

    printf("All wire-shape tests passed.\n");
    return 0;
}
