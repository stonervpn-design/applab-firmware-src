#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

/* Include the implementation directly for pure host testing */
#include "../applications/main/infrared/infrared_tvbgone_db.h"
#include "../applications/main/infrared/infrared_tvbgone_db.c"

/**
 * Test 1: Load database and verify embedded codes are available
 */
static void test_load_db(void) {
    printf("TEST 1: Load database... ");

    uint16_t count = infrared_tvbgone_load_db();
    assert(count > 0);
    assert(count == 7); // We have 7 embedded codes

    printf("OK (count=%u)\n", count);
}

/**
 * Test 2: Get code count
 */
static void test_get_code_count(void) {
    printf("TEST 2: Get code count... ");

    uint16_t count = infrared_tvbgone_get_code_count();
    assert(count == 7);

    printf("OK (count=%u)\n", count);
}

/**
 * Test 3: Iterator returns codes in order then NULL
 */
static void test_iterator_sequence(void) {
    printf("TEST 3: Iterator sequence... ");

    infrared_tvbgone_iter_reset();

    int code_count = 0;
    const IrUniversalCode* code;
    while ((code = infrared_tvbgone_iter_next()) != NULL) {
        assert(code->brand != NULL);
        assert(code->model != NULL);
        assert(code->code_name != NULL);
        assert(code->pattern != NULL);
        assert(code->pattern_len > 0);
        assert(code->frequency > 0);
        code_count++;
    }

    assert(code_count == 7);

    // Verify next call returns NULL
    code = infrared_tvbgone_iter_next();
    assert(code == NULL);

    printf("OK (iterated %d codes)\n", code_count);
}

/**
 * Test 4: Iterator can be reset and restarted
 */
static void test_iterator_reset(void) {
    printf("TEST 4: Iterator reset... ");

    // First pass
    infrared_tvbgone_iter_reset();
    const IrUniversalCode* first_code_1 = infrared_tvbgone_iter_next();
    assert(first_code_1 != NULL);
    assert(strcmp(first_code_1->brand, "Sony") == 0);

    // Consume remaining
    const IrUniversalCode* code;
    while ((code = infrared_tvbgone_iter_next()) != NULL) {
        // Consume
    }

    // Reset and check we get the same first code again
    infrared_tvbgone_iter_reset();
    const IrUniversalCode* first_code_2 = infrared_tvbgone_iter_next();
    assert(first_code_2 != NULL);
    assert(strcmp(first_code_2->brand, "Sony") == 0);
    assert(first_code_1 == first_code_2); // Should be same memory location

    printf("OK (reset successful)\n");
}

/**
 * Test 5: Get code by index
 */
static void test_get_code_by_index(void) {
    printf("TEST 5: Get code by index... ");

    const IrUniversalCode* code0 = infrared_tvbgone_get_code(0);
    assert(code0 != NULL);
    assert(strcmp(code0->brand, "Sony") == 0);

    const IrUniversalCode* code1 = infrared_tvbgone_get_code(1);
    assert(code1 != NULL);
    assert(strcmp(code1->brand, "LG") == 0);

    const IrUniversalCode* code6 = infrared_tvbgone_get_code(6);
    assert(code6 != NULL);
    assert(strcmp(code6->brand, "Toshiba") == 0);

    // Out of range should return NULL
    const IrUniversalCode* code_oob = infrared_tvbgone_get_code(7);
    assert(code_oob == NULL);

    const IrUniversalCode* code_oob2 = infrared_tvbgone_get_code(100);
    assert(code_oob2 == NULL);

    printf("OK (index access works)\n");
}

/**
 * Test 6: Brands list
 */
static void test_brands_list(void) {
    printf("TEST 6: Brands list... ");

    uint16_t brand_count = 0;
    const char** brands = infrared_tvbgone_brands(&brand_count);

    assert(brands != NULL);
    assert(brand_count == 7); // Each code has a different brand in our embedded set

    // Check that each brand is present
    bool found_sony = false;
    bool found_lg = false;
    bool found_samsung = false;

    for (uint16_t i = 0; i < brand_count; i++) {
        assert(brands[i] != NULL);
        if (strcmp(brands[i], "Sony") == 0) found_sony = true;
        if (strcmp(brands[i], "LG") == 0) found_lg = true;
        if (strcmp(brands[i], "Samsung") == 0) found_samsung = true;
    }

    assert(found_sony);
    assert(found_lg);
    assert(found_samsung);

    printf("OK (found %u brands)\n", brand_count);
}

/**
 * Test 7: Brands list with NULL out_count should be safe
 */
static void test_brands_list_null_param(void) {
    printf("TEST 7: Brands list with NULL param... ");

    const char** brands = infrared_tvbgone_brands(NULL);
    assert(brands == NULL);

    printf("OK (null handling)\n");
}

/**
 * Test 8: All embedded codes have valid patterns
 */
static void test_pattern_validity(void) {
    printf("TEST 8: Pattern validity... ");

    for (uint16_t i = 0; i < infrared_tvbgone_get_code_count(); i++) {
        const IrUniversalCode* code = infrared_tvbgone_get_code(i);
        assert(code != NULL);
        assert(code->pattern != NULL);
        assert(code->pattern_len > 0);
        assert(code->pattern_len <= 1000); // Sanity check

        // Check pattern contains reasonable timing values (10 µs to 100 ms)
        for (uint16_t j = 0; j < code->pattern_len; j++) {
            assert(code->pattern[j] > 0);
            assert(code->pattern[j] < 200000); // 200 ms max for any timing
        }
    }

    printf("OK (all patterns valid)\n");
}

/**
 * Test 9: Frequency values are reasonable
 */
static void test_frequency_validity(void) {
    printf("TEST 9: Frequency validity... ");

    for (uint16_t i = 0; i < infrared_tvbgone_get_code_count(); i++) {
        const IrUniversalCode* code = infrared_tvbgone_get_code(i);
        assert(code != NULL);
        // IR frequencies typically 30-40 kHz, with some exceptions
        assert(code->frequency >= 30000 && code->frequency <= 50000);
    }

    printf("OK (all frequencies in valid range)\n");
}

/**
 * Test 10: Iterator and index-based access return same data
 */
static void test_iterator_vs_index_consistency(void) {
    printf("TEST 10: Iterator vs index consistency... ");

    infrared_tvbgone_iter_reset();
    uint16_t idx = 0;
    const IrUniversalCode* iter_code;

    while ((iter_code = infrared_tvbgone_iter_next()) != NULL) {
        const IrUniversalCode* idx_code = infrared_tvbgone_get_code(idx);
        assert(idx_code != NULL);
        assert(iter_code == idx_code); // Should be same memory address
        idx++;
    }

    printf("OK (consistency verified)\n");
}

int main(void) {
    printf("=== TV-B-Gone Core Database Tests ===\n\n");

    test_load_db();
    test_get_code_count();
    test_iterator_sequence();
    test_iterator_reset();
    test_get_code_by_index();
    test_brands_list();
    test_brands_list_null_param();
    test_pattern_validity();
    test_frequency_validity();
    test_iterator_vs_index_consistency();

    printf("\n=== All tests passed ===\n");
    return 0;
}
