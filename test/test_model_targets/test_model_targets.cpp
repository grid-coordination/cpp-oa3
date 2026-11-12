#include <oa3/model/targets.h>
#include <string.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

static oa3::TargetList make(const char* const* items, size_t n, uint8_t non_string = 0) {
    oa3::TargetList t{};
    for (size_t i = 0; i < n; ++i) {
        strncpy(t.items[i], items[i], OA3_TARGET_MAX);
    }
    t.count = static_cast<uint8_t>(n);
    t.non_string_count = non_string;
    return t;
}

static void test_untargeted_matches_any_ven() {
    oa3::TargetList obj{};
    const char* ven[] = {"ven_0999"};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, ven, 1));
    TEST_ASSERT_TRUE(oa3::targets_match(obj, nullptr, 0));
    oa3::TargetList empty_ven{};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, empty_ven));
}

// User_Guide.md:2702-2754 (Example 8.13-4/5)
static void test_ven_name_target_matches() {
    const char* items[] = {"ven_0999"};
    oa3::TargetList obj = make(items, 1);
    const char* ven[] = {"ven_0999"};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, ven, 1));
    TEST_ASSERT_TRUE(oa3::targets_match(obj, make(ven, 1)));
}

static void test_disjoint_targets_do_not_match() {
    const char* items[] = {"group1"};
    oa3::TargetList obj = make(items, 1);
    const char* ven[] = {"areaX"};
    TEST_ASSERT_FALSE(oa3::targets_match(obj, ven, 1));
    TEST_ASSERT_FALSE(oa3::targets_match(obj, make(ven, 1)));
}

static void test_case_sensitive() {
    const char* items[] = {"Group1"};
    oa3::TargetList obj = make(items, 1);
    const char* ven[] = {"group1"};
    TEST_ASSERT_FALSE(oa3::targets_match(obj, ven, 1));
    TEST_ASSERT_FALSE(oa3::targets_contain(obj, "group1"));
}

static void test_one_overlap_of_many_matches() {
    const char* items[] = {"group1", "areaX", "ven_0001"};
    oa3::TargetList obj = make(items, 3);
    const char* ven[] = {"ven_0999", "zone7", "areaX"};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, ven, 3));
    TEST_ASSERT_TRUE(oa3::targets_match(obj, make(ven, 3)));
}

static void test_non_string_only_targets_do_not_match() {
    oa3::TargetList obj{};
    obj.non_string_count = 2;
    const char* ven[] = {"ven_0999"};
    TEST_ASSERT_FALSE(oa3::targets_match(obj, ven, 1));
    TEST_ASSERT_FALSE(oa3::targets_match(obj, nullptr, 0));
    TEST_ASSERT_FALSE(oa3::targets_match(obj, make(ven, 1)));
}

static void test_mixed_string_and_non_string_targets() {
    const char* items[] = {"group1"};
    oa3::TargetList obj = make(items, 1, 1);
    const char* hit[] = {"group1"};
    const char* miss[] = {"areaX"};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, hit, 1));
    TEST_ASSERT_FALSE(oa3::targets_match(obj, miss, 1));
}

static void test_targeted_object_against_empty_ven() {
    const char* items[] = {"group1"};
    oa3::TargetList obj = make(items, 1);
    TEST_ASSERT_FALSE(oa3::targets_match(obj, nullptr, 0));
    const char* ven[] = {"group1"};
    TEST_ASSERT_FALSE(oa3::targets_match(obj, ven, 0));
    oa3::TargetList empty_ven{};
    TEST_ASSERT_FALSE(oa3::targets_match(obj, empty_ven));
}

static void test_null_ven_entries_skipped() {
    const char* items[] = {"group1"};
    oa3::TargetList obj = make(items, 1);
    const char* ven[] = {nullptr, "group1"};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, ven, 2));
    const char* only_null[] = {nullptr};
    TEST_ASSERT_FALSE(oa3::targets_match(obj, only_null, 1));
}

static void test_empty_string_target() {
    const char* items[] = {""};
    oa3::TargetList obj = make(items, 1);
    const char* ven[] = {""};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, ven, 1));
    const char* other[] = {"x"};
    TEST_ASSERT_FALSE(oa3::targets_match(obj, other, 1));
}

static void test_prefix_is_not_a_match() {
    const char* items[] = {"ven_09"};
    oa3::TargetList obj = make(items, 1);
    const char* ven[] = {"ven_0999"};
    TEST_ASSERT_FALSE(oa3::targets_match(obj, ven, 1));
}

static void test_max_length_target() {
    char longest[OA3_TARGET_MAX + 1];
    memset(longest, 'a', OA3_TARGET_MAX);
    longest[OA3_TARGET_MAX] = '\0';
    const char* items[] = {longest};
    oa3::TargetList obj = make(items, 1);
    TEST_ASSERT_TRUE(oa3::targets_contain(obj, longest));
    const char* ven[] = {longest};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, ven, 1));
}

static void test_full_capacity_last_item_matches() {
    oa3::TargetList obj{};
    for (size_t i = 0; i < OA3_MAX_TARGETS; ++i) {
        obj.items[i][0] = static_cast<char>('a' + i);
    }
    obj.count = OA3_MAX_TARGETS;
    char last[2] = {static_cast<char>('a' + OA3_MAX_TARGETS - 1), '\0'};
    TEST_ASSERT_TRUE(oa3::targets_contain(obj, last));
    const char* ven[] = {last};
    TEST_ASSERT_TRUE(oa3::targets_match(obj, ven, 1));
}

static void test_contain() {
    const char* items[] = {"group1", "areaX"};
    oa3::TargetList t = make(items, 2);
    TEST_ASSERT_TRUE(oa3::targets_contain(t, "group1"));
    TEST_ASSERT_TRUE(oa3::targets_contain(t, "areaX"));
    TEST_ASSERT_FALSE(oa3::targets_contain(t, "group"));
    TEST_ASSERT_FALSE(oa3::targets_contain(t, "areax"));
    TEST_ASSERT_FALSE(oa3::targets_contain(t, nullptr));
    oa3::TargetList empty{};
    TEST_ASSERT_FALSE(oa3::targets_contain(empty, ""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_untargeted_matches_any_ven);
    RUN_TEST(test_ven_name_target_matches);
    RUN_TEST(test_disjoint_targets_do_not_match);
    RUN_TEST(test_case_sensitive);
    RUN_TEST(test_one_overlap_of_many_matches);
    RUN_TEST(test_non_string_only_targets_do_not_match);
    RUN_TEST(test_mixed_string_and_non_string_targets);
    RUN_TEST(test_targeted_object_against_empty_ven);
    RUN_TEST(test_null_ven_entries_skipped);
    RUN_TEST(test_empty_string_target);
    RUN_TEST(test_prefix_is_not_a_match);
    RUN_TEST(test_max_length_target);
    RUN_TEST(test_full_capacity_last_item_matches);
    RUN_TEST(test_contain);
    return UNITY_END();
}
