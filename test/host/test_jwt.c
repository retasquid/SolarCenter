#include "unity.h"
#include "jwt_exp.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* En-têtes et charges utiles construits pour le test, encodés en base64url sans remplissage :
 *   {"alg":"none"}                                 → eyJhbGciOiJub25lIn0
 *   {"exp":1798761600,"username":"x"}              → eyJleHAiOjE3OTg3NjE2MDAsInVzZXJuYW1lIjoieCJ9
 *   {"iss":"y", "exp" : 1700000000}                → eyJpc3MiOiJ5IiwgImV4cCIgOiAxNzAwMDAwMDAwfQ
 *   {"iss":"y"}                                    → eyJpc3MiOiJ5In0 */

static void test_reads_exp(void)
{
    TEST_ASSERT_EQUAL_INT64(1798761600,
        jwt_exp("eyJhbGciOiJub25lIn0.eyJleHAiOjE3OTg3NjE2MDAsInVzZXJuYW1lIjoieCJ9.c2ln"));
}

static void test_reads_exp_with_spaces(void)
{
    TEST_ASSERT_EQUAL_INT64(1700000000,
        jwt_exp("eyJhbGciOiJub25lIn0.eyJpc3MiOiJ5IiwgImV4cCIgOiAxNzAwMDAwMDAwfQ.c2ln"));
}

static void test_missing_exp_returns_zero(void)
{
    TEST_ASSERT_EQUAL_INT64(0, jwt_exp("eyJhbGciOiJub25lIn0.eyJpc3MiOiJ5In0.c2ln"));
}

static void test_malformed_returns_zero(void)
{
    TEST_ASSERT_EQUAL_INT64(0, jwt_exp(""));
    TEST_ASSERT_EQUAL_INT64(0, jwt_exp("pas-un-jwt"));
    TEST_ASSERT_EQUAL_INT64(0, jwt_exp("a.b"));
    TEST_ASSERT_EQUAL_INT64(0, jwt_exp("eyJhbGciOiJub25lIn0.e$J4.c2ln"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reads_exp);
    RUN_TEST(test_reads_exp_with_spaces);
    RUN_TEST(test_missing_exp_returns_zero);
    RUN_TEST(test_malformed_returns_zero);
    return UNITY_END();
}
