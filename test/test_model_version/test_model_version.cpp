#include <oa3/model/version.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

static void test_spec_version() { TEST_ASSERT_EQUAL_STRING("3.1.0", oa3::spec_version()); }

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_spec_version);
    return UNITY_END();
}
