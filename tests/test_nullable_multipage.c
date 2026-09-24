/**
 * @file test_nullable_multipage.c
 * @brief A nullable column written in one call larger than one page must
 *        round-trip row for row.
 *
 * The writer splits a single write_batch call into column chunks at the page
 * size (1 MiB by default), so a nullable DOUBLE batch larger than 131072 rows
 * crosses that boundary. Values of a nullable column are stored packed - only
 * the present rows - and are placed again through the definition levels, so a
 * chunk split used to shift the packed stream: rows lost their values or took
 * their neighbour's, while the row count stayed correct.
 *
 * Page boundaries are internal, so the assertions here are on the observable
 * round trip: every row keeps its own value or its own null, across the whole
 * file including the internal boundary. A correct reader must also report the
 * row count (definition-level entries), not the packed value count.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <carquet/carquet.h>
#include <carquet/error.h>
#include <carquet/types.h>

#include "test_helpers.h"

#define TEST_NAME "nullable_multipage"

/* 1 MiB page target / 8 bytes per DOUBLE = 131072 values per column chunk, so
 * this batch is split into at least two chunks. */
#define N_ROWS 200000
#define NULL_EVERY 7

int main(void) {
    char path[512];
    carquet_test_temp_path(path, sizeof(path), "carquet_nullable_multipage");

    carquet_error_t err = CARQUET_ERROR_INIT;
    carquet_schema_t* schema = carquet_schema_create(&err);
    if (!schema) TEST_FAIL(TEST_NAME, "schema creation failed");

    if (carquet_schema_add_column(schema, "k", CARQUET_PHYSICAL_INT64, NULL,
                                  CARQUET_REPETITION_REQUIRED, 0, 0) != CARQUET_OK ||
        carquet_schema_add_column(schema, "score", CARQUET_PHYSICAL_DOUBLE, NULL,
                                  CARQUET_REPETITION_OPTIONAL, 0, 0) != CARQUET_OK) {
        carquet_schema_free(schema);
        TEST_FAIL(TEST_NAME, "schema add column failed");
    }

    int64_t* k      = malloc((size_t)N_ROWS * sizeof(*k));
    double*  packed = malloc((size_t)N_ROWS * sizeof(*packed));  /* present rows only */
    int16_t* def    = malloc((size_t)N_ROWS * sizeof(*def));
    if (!k || !packed || !def) {
        free(k); free(packed); free(def);
        carquet_schema_free(schema);
        TEST_FAIL(TEST_NAME, "allocation failed");
    }

    int64_t n_present = 0;
    for (int64_t i = 0; i < N_ROWS; i++) {
        int present = (i % NULL_EVERY) != 0;
        k[i] = i;
        def[i] = (int16_t)present;
        if (present) packed[n_present++] = (double)i * 0.5;
    }

    carquet_writer_options_t opts;
    carquet_writer_options_init(&opts);
    carquet_writer_t* writer = carquet_writer_create(path, schema, &opts, &err);
    if (!writer) {
        free(k); free(packed); free(def);
        carquet_schema_free(schema);
        TEST_FAIL(TEST_NAME, "writer creation failed");
    }

    carquet_status_t st = carquet_writer_write_batch(writer, 0, k, N_ROWS, NULL, NULL);
    if (st == CARQUET_OK) {
        st = carquet_writer_write_batch(writer, 1, packed, N_ROWS, def, NULL);
    }
    if (st == CARQUET_OK) {
        st = carquet_writer_close(writer);
    } else {
        carquet_status_t close_st = carquet_writer_close(writer);
        (void)close_st;   /* the write already failed; keep that status */
    }
    if (st != CARQUET_OK) {
        free(k); free(packed); free(def);
        carquet_schema_free(schema);
        carquet_test_cleanup(path);
        TEST_FAIL(TEST_NAME, "write failed");
    }

    carquet_reader_t* reader = carquet_reader_open(path, NULL, &err);
    if (!reader) {
        free(k); free(packed); free(def);
        carquet_schema_free(schema);
        carquet_test_cleanup(path);
        TEST_FAIL(TEST_NAME, "reader open failed");
    }
    if (carquet_reader_num_rows(reader) != N_ROWS) {
        carquet_reader_close(reader);
        free(k); free(packed); free(def);
        carquet_schema_free(schema);
        carquet_test_cleanup(path);
        TEST_FAIL(TEST_NAME, "row count mismatch");
    }

    carquet_column_reader_t* col = carquet_reader_get_column(reader, 0, 1, &err);
    if (!col) {
        carquet_reader_close(reader);
        free(k); free(packed); free(def);
        carquet_schema_free(schema);
        carquet_test_cleanup(path);
        TEST_FAIL(TEST_NAME, "column reader creation failed");
    }

    double*  got     = malloc((size_t)N_ROWS * sizeof(*got));
    int16_t* got_def = malloc((size_t)N_ROWS * sizeof(*got_def));
    if (!got || !got_def) {
        carquet_column_reader_free(col);
        carquet_reader_close(reader);
        free(k); free(packed); free(def); free(got); free(got_def);
        carquet_schema_free(schema);
        carquet_test_cleanup(path);
        TEST_FAIL(TEST_NAME, "read buffer allocation failed");
    }

    /* One batch covering the whole column: the return value counts rows, which
     * is more than the number of values written for a nullable column. */
    int64_t rows_read = carquet_column_read_batch(col, got, N_ROWS, got_def, NULL);
    if (rows_read != N_ROWS) {
        char msg[128];
        snprintf(msg, sizeof(msg), "read %lld rows, expected %d",
                 (long long)rows_read, N_ROWS);
        carquet_column_reader_free(col);
        carquet_reader_close(reader);
        free(k); free(packed); free(def); free(got); free(got_def);
        carquet_schema_free(schema);
        carquet_test_cleanup(path);
        TEST_FAIL(TEST_NAME, msg);
    }

    int64_t vi = 0;
    for (int64_t i = 0; i < N_ROWS; i++) {
        int present = (i % NULL_EVERY) != 0;
        if (got_def[i] != (int16_t)present) {
            char msg[128];
            snprintf(msg, sizeof(msg), "row %lld: def level %d, expected %d",
                     (long long)i, (int)got_def[i], present);
            carquet_column_reader_free(col);
            carquet_reader_close(reader);
            free(k); free(packed); free(def); free(got); free(got_def);
            carquet_schema_free(schema);
            carquet_test_cleanup(path);
            TEST_FAIL(TEST_NAME, msg);
        }
        if (present) {
            double want = (double)i * 0.5;
            if (got[vi] != want) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "row %lld: value %.1f, expected %.1f (packed index %lld)",
                         (long long)i, got[vi], want, (long long)vi);
                carquet_column_reader_free(col);
                carquet_reader_close(reader);
                free(k); free(packed); free(def); free(got); free(got_def);
                carquet_schema_free(schema);
                carquet_test_cleanup(path);
                TEST_FAIL(TEST_NAME, msg);
            }
            vi++;
        }
    }
    if (vi != n_present) {
        carquet_column_reader_free(col);
        carquet_reader_close(reader);
        free(k); free(packed); free(def); free(got); free(got_def);
        carquet_schema_free(schema);
        carquet_test_cleanup(path);
        TEST_FAIL(TEST_NAME, "packed value count mismatch");
    }

    /* The end of the column is still reported as such. */
    if (carquet_column_read_batch(col, got, N_ROWS, got_def, NULL) != 0) {
        carquet_column_reader_free(col);
        carquet_reader_close(reader);
        free(k); free(packed); free(def); free(got); free(got_def);
        carquet_schema_free(schema);
        carquet_test_cleanup(path);
        TEST_FAIL(TEST_NAME, "read past end of column did not return 0");
    }

    carquet_column_reader_free(col);
    carquet_reader_close(reader);
    free(k); free(packed); free(def); free(got); free(got_def);
    carquet_schema_free(schema);
    carquet_test_cleanup(path);

    TEST_PASS(TEST_NAME);
    return 0;
}
