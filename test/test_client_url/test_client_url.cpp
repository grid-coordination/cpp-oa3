#include <string.h>

#include <oa3/client/http_transport.h>
#include <oa3/client/limits.h>
#include <oa3/client/url.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

static oa3::TargetList targets_of(const char* const* items, size_t n) {
    oa3::TargetList t{};
    for (size_t i = 0; i < n; ++i) {
        strcpy(t.items[i], items[i]);
    }
    t.count = static_cast<uint8_t>(n);
    return t;
}

static void test_percent_encode() {
    char out[64];
    size_t len = 99;
    TEST_ASSERT_TRUE(oa3::percent_encode("AZaz09-._~", out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_STRING("AZaz09-._~", out);
    TEST_ASSERT_EQUAL_size_t(10, len);
    TEST_ASSERT_TRUE(oa3::percent_encode("a b/c?d&e=f+g%h", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a%20b%2Fc%3Fd%26e%3Df%2Bg%25h", out);
    TEST_ASSERT_TRUE(oa3::percent_encode("\xC3\xA9:", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("%C3%A9%3A", out);
    TEST_ASSERT_TRUE(oa3::percent_encode("", out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_size_t(0, len);
}

static void test_percent_encode_overflow() {
    char out[4];
    size_t len = 99;
    TEST_ASSERT_TRUE(oa3::percent_encode("abc", out, sizeof(out), &len));
    TEST_ASSERT_FALSE(oa3::percent_encode("abcd", out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_size_t(0, len);
    TEST_ASSERT_TRUE(oa3::percent_encode(" ", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("%20", out);
    TEST_ASSERT_FALSE(oa3::percent_encode("a ", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(oa3::percent_encode("", out, 0));
}

static void test_builder_basics() {
    char buf[128];
    oa3::UrlBuilder b(buf, sizeof(buf));
    TEST_ASSERT_TRUE(b.ok());
    TEST_ASSERT_EQUAL_STRING("", b.c_str());
    b.append("https://h/base").append_path("a b").append_query("x", "1&2").append_query("n", 4294967295u);
    TEST_ASSERT_TRUE(b.ok());
    TEST_ASSERT_EQUAL_STRING("https://h/base/a%20b?x=1%262&n=4294967295", buf);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), b.length());
}

static void test_builder_zero_size() {
    char buf[1] = {'z'};
    oa3::UrlBuilder b(buf, 0);
    TEST_ASSERT_FALSE(b.ok());
    b.append("x");
    TEST_ASSERT_EQUAL_CHAR('z', buf[0]);
}

static void test_builder_trailing_slash() {
    char buf[64];
    oa3::UrlBuilder b(buf, sizeof(buf));
    b.append("http://h/v/").append_path("events");
    TEST_ASSERT_EQUAL_STRING("http://h/v/events", buf);
}

static void test_builder_query_in_base() {
    char buf[64];
    oa3::UrlBuilder b(buf, sizeof(buf));
    b.append("http://h/v?key=1").append_query("a", "b");
    TEST_ASSERT_EQUAL_STRING("http://h/v?key=1&a=b", buf);
}

static void test_builder_overflow_is_sticky() {
    char buf[16];
    oa3::UrlBuilder b(buf, sizeof(buf));
    b.append("http://h").append_path("ab");
    TEST_ASSERT_TRUE(b.ok());
    TEST_ASSERT_EQUAL_STRING("http://h/ab", buf);
    b.append_query("name", "value");
    TEST_ASSERT_FALSE(b.ok());
    TEST_ASSERT_EQUAL_STRING("http://h/ab", buf);
    b.append("x");
    TEST_ASSERT_FALSE(b.ok());
    TEST_ASSERT_EQUAL_STRING("http://h/ab", buf);
    TEST_ASSERT_EQUAL_size_t(11, b.length());

    char small[12];
    oa3::UrlBuilder c(small, sizeof(small));
    c.append("http://h").append_path("a b");
    TEST_ASSERT_FALSE(c.ok());
    TEST_ASSERT_EQUAL_STRING("http://h", small);
    c.append("/");
    TEST_ASSERT_EQUAL_STRING("http://h", small);

    char exact[9];
    oa3::UrlBuilder d(exact, sizeof(exact));
    d.append("12345678");
    TEST_ASSERT_TRUE(d.ok());
    d.append("9");
    TEST_ASSERT_FALSE(d.ok());
    TEST_ASSERT_EQUAL_STRING("12345678", exact);
}

static void test_programs_url() {
    char buf[OA3_URL_MAX + 1];
    TEST_ASSERT_TRUE(oa3::build_programs_url(buf, sizeof(buf), "https://h/openadr3/3.1.0",
                                             "EELEC-012041131", nullptr, 0, 50));
    TEST_ASSERT_EQUAL_STRING(
        "https://h/openadr3/3.1.0/programs?programName=EELEC-012041131&skip=0&limit=50", buf);
    const char* items[] = {"group 1", "B"};
    oa3::TargetList t = targets_of(items, 2);
    TEST_ASSERT_TRUE(oa3::build_programs_url(buf, sizeof(buf), "https://h/v/", nullptr, &t, 100, 50));
    TEST_ASSERT_EQUAL_STRING("https://h/v/programs?targets=group%201&targets=B&skip=100&limit=50", buf);
    oa3::TargetList empty{};
    TEST_ASSERT_TRUE(oa3::build_programs_url(buf, sizeof(buf), "https://h/v", nullptr, &empty, 0, 1));
    TEST_ASSERT_EQUAL_STRING("https://h/v/programs?skip=0&limit=1", buf);
    TEST_ASSERT_FALSE(oa3::build_programs_url(buf, 20, "https://h/v", nullptr, nullptr, 0, 1));
    TEST_ASSERT_FALSE(oa3::build_programs_url(buf, sizeof(buf), nullptr, nullptr, nullptr, 0, 1));
}

static void test_program_url() {
    char buf[OA3_URL_MAX + 1];
    TEST_ASSERT_TRUE(oa3::build_program_url(buf, sizeof(buf), "https://h/v", "a/b c"));
    TEST_ASSERT_EQUAL_STRING("https://h/v/programs/a%2Fb%20c", buf);
}

static void test_events_url() {
    char buf[OA3_URL_MAX + 1];
    TEST_ASSERT_TRUE(oa3::build_events_url(buf, sizeof(buf), "https://h/v",
                                           "b54d3d1f-bc87-4e47-bbc1-2b95958283fc", nullptr, 0, 50));
    TEST_ASSERT_EQUAL_STRING(
        "https://h/v/events?programID=b54d3d1f-bc87-4e47-bbc1-2b95958283fc&skip=0&limit=50", buf);
    const char* items[] = {"A", "B&C", "D", "E"};
    oa3::TargetList t = targets_of(items, 4);
    TEST_ASSERT_TRUE(oa3::build_events_url(buf, sizeof(buf), "https://h/v", "p1", &t, 100, 50));
    TEST_ASSERT_EQUAL_STRING(
        "https://h/v/events?programID=p1&targets=A&targets=B%26C&targets=D&targets=E"
        "&skip=100&limit=50",
        buf);
}

static void test_is_object_id() {
    TEST_ASSERT_TRUE(oa3::is_object_id("b54d3d1f-bc87-4e47-bbc1-2b95958283fc"));
    TEST_ASSERT_TRUE(oa3::is_object_id("object_999"));
    TEST_ASSERT_FALSE(oa3::is_object_id(""));
    TEST_ASSERT_FALSE(oa3::is_object_id(nullptr));
    TEST_ASSERT_FALSE(oa3::is_object_id("a b"));
    TEST_ASSERT_FALSE(oa3::is_object_id("a/b"));
    TEST_ASSERT_FALSE(oa3::is_object_id("a%20"));
    char longest[130];
    memset(longest, 'x', 128);
    longest[128] = '\0';
    TEST_ASSERT_TRUE(oa3::is_object_id(longest));
    longest[128] = 'x';
    longest[129] = '\0';
    TEST_ASSERT_FALSE(oa3::is_object_id(longest));
}

static void test_to_string() {
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(oa3::HttpResult::kOk));
    TEST_ASSERT_EQUAL_STRING("UNSUPPORTED", oa3::to_string(oa3::HttpResult::kUnsupported));
    TEST_ASSERT_EQUAL_STRING("TOO_LARGE", oa3::to_string(oa3::HttpResult::kTooLarge));
    TEST_ASSERT_EQUAL_STRING("IO_ERROR", oa3::to_string(oa3::HttpResult::kIoError));
}


class GetOnly final : public oa3::HttpTransport {
public:
    oa3::HttpResult get(const oa3::HttpRequest&, oa3::HttpResponseFn, void*) override {
        return oa3::HttpResult::kOk;
    }
};

static void ignore_response(void*, int, oa3::ByteReader&) {}

static void test_post_unsupported_by_default() {
    GetOnly t;
    const oa3::HttpRequest req{"https://h/v/auth/token", nullptr, 0, 0};
    TEST_ASSERT_EQUAL_STRING(
        "UNSUPPORTED", oa3::to_string(t.post(req, "application/x-www-form-urlencoded", "a=b", 3,
                                             ignore_response, nullptr)));
}

static void test_notifier_urls() {
    char buf[256];
    const char* base = "https://vtn.example/openadr3/3.1.0";
    TEST_ASSERT_TRUE(oa3::build_notifiers_url(buf, sizeof(buf), base));
    TEST_ASSERT_EQUAL_STRING("https://vtn.example/openadr3/3.1.0/notifiers", buf);
    TEST_ASSERT_TRUE(oa3::build_program_events_topics_url(buf, sizeof(buf), base, "p-1"));
    TEST_ASSERT_EQUAL_STRING(
        "https://vtn.example/openadr3/3.1.0/notifiers/mqtt/topics/programs/p-1/events", buf);
    TEST_ASSERT_TRUE(oa3::build_program_topics_url(buf, sizeof(buf), base, "p-1"));
    TEST_ASSERT_EQUAL_STRING("https://vtn.example/openadr3/3.1.0/notifiers/mqtt/topics/programs/p-1",
                             buf);
    TEST_ASSERT_TRUE(oa3::build_notifiers_url(buf, sizeof(buf), "https://vtn.example/"));
    TEST_ASSERT_EQUAL_STRING("https://vtn.example/notifiers", buf);
    TEST_ASSERT_FALSE(oa3::build_notifiers_url(buf, sizeof(buf), nullptr));
    TEST_ASSERT_FALSE(oa3::build_program_topics_url(buf, sizeof(buf), base, nullptr));
    TEST_ASSERT_FALSE(oa3::build_program_events_topics_url(buf, 60, base, "p-1"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_percent_encode);
    RUN_TEST(test_percent_encode_overflow);
    RUN_TEST(test_builder_basics);
    RUN_TEST(test_builder_zero_size);
    RUN_TEST(test_builder_trailing_slash);
    RUN_TEST(test_builder_query_in_base);
    RUN_TEST(test_builder_overflow_is_sticky);
    RUN_TEST(test_programs_url);
    RUN_TEST(test_program_url);
    RUN_TEST(test_events_url);
    RUN_TEST(test_is_object_id);
    RUN_TEST(test_post_unsupported_by_default);
    RUN_TEST(test_notifier_urls);
    RUN_TEST(test_to_string);
    return UNITY_END();
}
