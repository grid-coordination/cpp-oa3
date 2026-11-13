#include <string.h>

#include <oa3/client/log.h>
#include <unity.h>

static int calls;
static oa3::LogLevel last_level;
static char last_line[512];
static void* last_ctx;

void setUp() {
    calls = 0;
    last_line[0] = '\0';
    last_ctx = nullptr;
}
void tearDown() {}

static void sink_fn(void* ctx, oa3::LogLevel level, const char* line) {
    ++calls;
    last_ctx = ctx;
    last_level = level;
    strcpy(last_line, line);
}

static void test_level_filter() {
    int tag = 0;
    oa3::LogSink sink{sink_fn, &tag, oa3::LogLevel::kInfo};
    oa3::log_printf(sink, oa3::LogLevel::kDebug, "hidden %d", 1);
    TEST_ASSERT_EQUAL_INT(0, calls);
    oa3::log_printf(sink, oa3::LogLevel::kInfo, "shown %d", 2);
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_STRING("shown 2", last_line);
    TEST_ASSERT_EQUAL_PTR(&tag, last_ctx);
    TEST_ASSERT_TRUE(last_level == oa3::LogLevel::kInfo);
    oa3::log_printf(sink, oa3::LogLevel::kError, "error");
    TEST_ASSERT_EQUAL_INT(2, calls);
    TEST_ASSERT_TRUE(last_level == oa3::LogLevel::kError);
}

static void test_truncation() {
    oa3::LogSink sink{sink_fn, nullptr, oa3::LogLevel::kDebug};
    char long_text[400];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    oa3::log_printf(sink, oa3::LogLevel::kWarn, "%s", long_text);
    TEST_ASSERT_EQUAL_size_t(OA3_LOG_LINE_MAX, strlen(last_line));
    TEST_ASSERT_EQUAL_CHAR('a', last_line[OA3_LOG_LINE_MAX - 1]);
}

static void test_null_fn() {
    oa3::LogSink sink{};
    oa3::log_printf(sink, oa3::LogLevel::kError, "nothing");
    oa3::LogSink sink2{nullptr, nullptr, oa3::LogLevel::kDebug};
    oa3::log_printf(sink2, oa3::LogLevel::kError, "nothing");
    TEST_ASSERT_EQUAL_INT(0, calls);
}

static void test_to_string() {
    TEST_ASSERT_EQUAL_STRING("ERROR", oa3::to_string(oa3::LogLevel::kError));
    TEST_ASSERT_EQUAL_STRING("DEBUG", oa3::to_string(oa3::LogLevel::kDebug));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_level_filter);
    RUN_TEST(test_truncation);
    RUN_TEST(test_null_fn);
    RUN_TEST(test_to_string);
    return UNITY_END();
}
