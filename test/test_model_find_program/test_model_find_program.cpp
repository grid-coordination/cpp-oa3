#include <stdio.h>
#include <string.h>

#include <oa3/model/json_arena.h>
#include <oa3/model/parse.h>
#include <unity.h>

using oa3::ParseResult;

static oa3::StaticJsonArena<> arena;
static oa3::Program program;
static char page[16384];

void setUp() { memset(&program, 0, sizeof(program)); }
void tearDown() {}

#define ASSERT_RESULT(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

// count programs named P-<i> with id id-<i>.
static const char* make_page(unsigned count) {
    size_t n = 0;
    n += static_cast<size_t>(snprintf(page + n, sizeof(page) - n, "["));
    for (unsigned i = 0; i < count; ++i) {
        n += static_cast<size_t>(snprintf(
            page + n, sizeof(page) - n,
            "%s{\"id\":\"id-%u\",\"programName\":\"P-%u\",\"objectType\":\"PROGRAM\","
            "\"payloadDescriptors\":[{\"payloadType\":\"PRICE\",\"units\":\"KWH\",\"currency\":\"USD\"}]}",
            i == 0 ? "" : ",", i, i));
    }
    snprintf(page + n, sizeof(page) - n, "]");
    return page;
}

static ParseResult find(const char* json, const char* name, bool* found, size_t* scanned) {
    return oa3::find_program(json, strlen(json), arena, name, &program, found, scanned);
}

static void check_match_at(unsigned index) {
    char name[16];
    char id[16];
    snprintf(name, sizeof(name), "P-%u", index);
    snprintf(id, sizeof(id), "id-%u", index);
    bool found = false;
    size_t scanned = 0;
    ASSERT_RESULT(ParseResult::kOk, find(make_page(50), name, &found, &scanned));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_size_t(50, scanned);
    TEST_ASSERT_EQUAL_STRING(id, program.id);
    TEST_ASSERT_EQUAL_STRING(name, program.program_name);
    TEST_ASSERT_EQUAL_UINT8(1, program.descriptor_count);
    TEST_ASSERT_EQUAL_STRING("USD", program.descriptors[0].currency);
}

static void test_match_first() { check_match_at(0); }
static void test_match_middle() { check_match_at(25); }
static void test_match_last() { check_match_at(49); }

static void test_no_match() {
    bool found = true;
    size_t scanned = 0;
    ASSERT_RESULT(ParseResult::kOk, find(make_page(7), "P-99", &found, &scanned));
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_size_t(7, scanned);
    TEST_ASSERT_EQUAL_STRING("", program.id);
}

static void test_skips_bad_names_in_other_elements() {
    char json[1024];
    snprintf(json, sizeof(json),
             "[{\"id\":\"a\",\"programName\":\"%0100d\"},"
             "{\"id\":\"b\"},"
             "{\"id\":\"c\",\"programName\":7},"
             "{\"id\":\"d\",\"program_name\":\"WANTED\"}]",
             0);
    bool found = false;
    size_t scanned = 0;
    ASSERT_RESULT(ParseResult::kOk, find(json, "WANTED", &found, &scanned));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_size_t(4, scanned);
    TEST_ASSERT_EQUAL_STRING("d", program.id);
}

static void test_first_match_kept() {
    const char* json =
        R"([{"id":"one","programName":"X"},{"id":"two","programName":"X"}])";
    bool found = false;
    size_t scanned = 0;
    ASSERT_RESULT(ParseResult::kOk, find(json, "X", &found, &scanned));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_size_t(2, scanned);
    TEST_ASSERT_EQUAL_STRING("one", program.id);
}

static void test_match_with_invalid_field_fails() {
    const char* json = R"([{"programName":"X"}])";
    bool found = true;
    size_t scanned = 9;
    ASSERT_RESULT(ParseResult::kInvalidField, find(json, "X", &found, &scanned));
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_size_t(0, scanned);
}

static void test_empty_array() {
    bool found = true;
    size_t scanned = 9;
    ASSERT_RESULT(ParseResult::kOk, find(" [ ] ", "X", &found, &scanned));
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_size_t(0, scanned);
}

static void test_not_an_array() {
    bool found = false;
    size_t scanned = 0;
    ASSERT_RESULT(ParseResult::kInvalidField, find(R"({"id":"a","programName":"X"})", "X", &found, &scanned));
    ASSERT_RESULT(ParseResult::kInvalidField, find("\"X\"", "X", &found, &scanned));
    ASSERT_RESULT(ParseResult::kInvalidField, find("[1]", "X", &found, &scanned));
}

static void test_malformed() {
    bool found = false;
    size_t scanned = 9;
    ASSERT_RESULT(ParseResult::kJsonError, find("", "X", &found, &scanned));
    ASSERT_RESULT(ParseResult::kJsonError, find("<html>", "X", &found, &scanned));
    ASSERT_RESULT(ParseResult::kJsonError,
                  find(R"([{"id":"a","programName":"Y"},{"id":"b","progr)", "X", &found, &scanned));
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_size_t(1, scanned);
    ASSERT_RESULT(ParseResult::kJsonError,
                  find(R"([{"id":"a","programName":"Y"} {"id":"b"}])", "X", &found, &scanned));
    TEST_ASSERT_EQUAL_size_t(1, scanned);
}

static void test_price_server_page() {
    FILE* f = fopen(OA3_FIXTURE_DIR "/price_server_programs.json", "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t n = fread(page, 1, sizeof(page) - 1, f);
    fclose(f);
    page[n] = '\0';
    bool found = false;
    size_t scanned = 0;
    ASSERT_RESULT(ParseResult::kOk,
                  oa3::find_program(page, n, arena, "EELEC-013532223", &program, &found, &scanned));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_size_t(1, scanned);
    TEST_ASSERT_EQUAL_STRING("b54d3d1f-bc87-4e47-bbc1-2b95958283fc", program.id);
    TEST_ASSERT_EQUAL_STRING("KWH", program.descriptors[0].units);
    ASSERT_RESULT(ParseResult::kOk,
                  oa3::find_program(page, n, arena, "EELEC-012041131", &program, &found, &scanned));
    TEST_ASSERT_FALSE(found);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_match_first);
    RUN_TEST(test_match_middle);
    RUN_TEST(test_match_last);
    RUN_TEST(test_no_match);
    RUN_TEST(test_skips_bad_names_in_other_elements);
    RUN_TEST(test_first_match_kept);
    RUN_TEST(test_match_with_invalid_field_fails);
    RUN_TEST(test_empty_array);
    RUN_TEST(test_not_an_array);
    RUN_TEST(test_malformed);
    RUN_TEST(test_price_server_page);
    return UNITY_END();
}
