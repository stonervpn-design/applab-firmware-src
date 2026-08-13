#include "ducky_os_cond.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Run a script (NULL-terminated line array) with target_os; collect the
 * concatenated first-chars of EXECUTE lines into out (for easy assertions). */
static void run(const char** lines, DuckyOs os, char* out) {
    DuckyOsCondState st; ducky_os_cond_reset(&st, os);
    int o = 0;
    for(int i = 0; lines[i]; i++) {
        DuckyCondResult r = ducky_os_cond_eval(&st, lines[i]);
        if(r == DUCKY_COND_EXECUTE) out[o++] = lines[i][0];
    }
    out[o] = 0;
}

static void test_basic_if(void) {
    const char* s[] = {"IF_OS WINDOWS","Wline","ELSE_OS","Lline","END_IF","after",0};
    char out[32];
    run(s, DUCKY_OS_WINDOWS, out); assert(strcmp(out,"Wa")==0); /* Wline + after */
    run(s, DUCKY_OS_LINUX, out);   assert(strcmp(out,"La")==0); /* Lline + after */
    printf("test_basic_if OK\n");
}

static void test_no_else(void) {
    const char* s[] = {"IF_OS MAC","Mline","END_IF","x",0};
    char out[32];
    run(s, DUCKY_OS_MAC, out);     assert(strcmp(out,"Mx")==0);
    run(s, DUCKY_OS_WINDOWS, out); assert(strcmp(out,"x")==0);
    printf("test_no_else OK\n");
}

static void test_nested(void) {
    const char* s[] = {"IF_OS WINDOWS","A","IF_OS MAC","B","ELSE_OS","C","END_IF","D","END_IF","E",0};
    char out[32];
    /* target WINDOWS: outer matches; inner MAC != WINDOWS -> C; so A,C,D,E */
    run(s, DUCKY_OS_WINDOWS, out); assert(strcmp(out,"ACDE")==0);
    /* target MAC: outer WINDOWS!=MAC -> whole outer skipped -> only E */
    run(s, DUCKY_OS_MAC, out);     assert(strcmp(out,"E")==0);
    printf("test_nested OK\n");
}

static void test_set_os(void) {
    const char* s[] = {"SET_OS LINUX","IF_OS LINUX","L","END_IF",0};
    char out[32]; run(s, DUCKY_OS_WINDOWS, out); assert(strcmp(out,"L")==0);
    printf("test_set_os OK\n");
}

static void test_malformed_and_unbalanced(void) {
    DuckyOsCondState st; ducky_os_cond_reset(&st, DUCKY_OS_WINDOWS);
    assert(ducky_os_cond_eval(&st, "END_IF") == DUCKY_COND_ERROR);   /* unbalanced */
    assert(ducky_os_cond_eval(&st, "ELSE_OS") == DUCKY_COND_ERROR);  /* unbalanced */
    assert(ducky_os_cond_eval(&st, "SET_OS ZZZ") == DUCKY_COND_ERROR); /* bad os */
    /* near-miss token is NOT a directive (fresh state, depth 0 -> EXECUTE) */
    ducky_os_cond_reset(&st, DUCKY_OS_WINDOWS);
    assert(ducky_os_cond_eval(&st, "IF_OSX WINDOWS") == DUCKY_COND_EXECUTE);
    printf("test_malformed_and_unbalanced OK\n");
}

/* A malformed IF_OS must FAIL CLOSED: it still pushes a never-matching frame so
 * the guarded body is SKIPPED (not run on every OS) and END_IF stays balanced. */
static void test_bad_if_os_fails_closed(void) {
    const char* s[] = {"IF_OS MACOSX", "danger", "END_IF", "after", 0};
    char out[32]; run(s, DUCKY_OS_LINUX, out);
    assert(strcmp(out, "a") == 0); /* only "after" runs; "danger" skipped (run() = first chars) */
    /* the IF_OS returns ERROR (typo logged) but the stack still balances */
    DuckyOsCondState st; ducky_os_cond_reset(&st, DUCKY_OS_LINUX);
    assert(ducky_os_cond_eval(&st, "IF_OS BSD") == DUCKY_COND_ERROR);
    assert(ducky_os_cond_eval(&st, "x") == DUCKY_COND_SKIP);        /* body skipped */
    assert(ducky_os_cond_eval(&st, "END_IF") == DUCKY_COND_DIRECTIVE); /* balanced pop */
    assert(ducky_os_cond_eval(&st, "y") == DUCKY_COND_EXECUTE);     /* back to active */
    printf("test_bad_if_os_fails_closed OK\n");
}

static void test_overnest(void) {
    DuckyOsCondState st; ducky_os_cond_reset(&st, DUCKY_OS_WINDOWS);
    for(int i = 0; i < DUCKY_OS_COND_MAX_DEPTH; i++)
        assert(ducky_os_cond_eval(&st, "IF_OS WINDOWS") == DUCKY_COND_DIRECTIVE);
    assert(ducky_os_cond_eval(&st, "IF_OS WINDOWS") == DUCKY_COND_ERROR); /* depth exceeded */
    printf("test_overnest OK\n");
}

int main(void) {
    test_basic_if(); test_no_else(); test_nested(); test_set_os();
    test_malformed_and_unbalanced(); test_overnest();
    test_bad_if_os_fails_closed();
    printf("test_ducky_os_cond: ALL PASS\n");
    return 0;
}
