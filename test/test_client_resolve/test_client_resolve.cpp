#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using fakes::kBaseUrl;
using fakes::kProgramId;
using fakes::kProgramName;
using oa3::VenStatus;

static fakes::FakeTransport http;
static fakes::FakeClock clk;
static fakes::Recorder rec;
static oa3::StaticVenStorage<> storage;
static oa3::VenClient client(http, clk, storage.storage());

static char programs_fixture[4096];
static char program_object[4096];
static char pages[3][16384];

#define ASSERT_STATUS(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() {
    http.reset();
    clk = fakes::FakeClock{};
    clk.set("2026-11-13T20:00:00Z");
    clk.ms = 1000;
    rec.reset();
    client.stop();
}
void tearDown() {}

static oa3::VenConfig config() {
    oa3::VenConfig c{};
    c.base_url = kBaseUrl;
    c.program_name = kProgramName;
    c.on_status = fakes::Recorder::on_status;
    c.on_price_change = fakes::Recorder::on_price;
    c.ctx = &rec;
    return c;
}

static void test_server_honors_program_name() {
    http.push_ok(programs_fixture,
                 "/programs?programName=EELEC-013532223&skip=0&limit=50");
    TEST_ASSERT_TRUE(client.start(config()));
    TEST_ASSERT_EQUAL_size_t(0, rec.statuses);
    TEST_ASSERT_NULL(client.program());
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_NOT_NULL(client.program());
    TEST_ASSERT_EQUAL_STRING(kProgramId, client.program()->id);
    ASSERT_STATUS(VenStatus::kResolvingProgram, client.status().status);
    TEST_ASSERT_EQUAL_size_t(1, rec.statuses);
    TEST_ASSERT_EQUAL_UINT32(0, client.ms_until_due());
    http.push_ok("[]", "/events?programID=b54d3d1f-bc87-4e47-bbc1-2b95958283fc");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
}

static void test_server_ignores_program_name() {
    http.push_ok(fakes::programs_page(pages[0], sizeof(pages[0]), "A", 0, 50), "&skip=0&limit=50");
    http.push_ok(fakes::programs_page(pages[1], sizeof(pages[1]), "A", 50, 50), "&skip=50&limit=50");
    http.push_ok(fakes::programs_page(pages[2], sizeof(pages[2]), "A", 100, 50, 7, kProgramName),
                 "&skip=100&limit=50");
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    TEST_ASSERT_NULL(client.program());
    ASSERT_STATUS(VenStatus::kResolvingProgram, client.status().status);
    TEST_ASSERT_EQUAL_UINT32(0, client.ms_until_due());
    client.loop();
    TEST_ASSERT_NULL(client.program());
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_NOT_NULL(client.program());
    TEST_ASSERT_EQUAL_STRING(kProgramId, client.program()->id);
    TEST_ASSERT_EQUAL_size_t(1, rec.statuses);
}

static void test_found_on_page_two() {
    http.push_ok(fakes::programs_page(pages[0], sizeof(pages[0]), "A", 0, 50), "&skip=0&");
    http.push_ok(fakes::programs_page(pages[1], sizeof(pages[1]), "A", 50, 3, 2, kProgramName),
                 "&skip=50&");
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_NOT_NULL(client.program());
}

static void test_not_found_then_retry_after_poll_period() {
    http.push_ok(fakes::programs_page(pages[0], sizeof(pages[0]), "A", 0, 50), "&skip=0&");
    http.push_ok(fakes::programs_page(pages[1], sizeof(pages[1]), "A", 50, 10), "&skip=50&");
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    client.loop();
    ASSERT_STATUS(VenStatus::kProgramNotFound, client.status().status);
    TEST_ASSERT_EQUAL_size_t(2, rec.statuses);
    ASSERT_STATUS(VenStatus::kProgramNotFound, rec.last_status().status);
    TEST_ASSERT_EQUAL_UINT16(0, rec.last_status().consecutive_failures);
    TEST_ASSERT_EQUAL_UINT32(900000, rec.last_status().retry_in_ms);
    TEST_ASSERT_EQUAL_UINT32(900000, client.ms_until_due());
    client.loop();
    clk.advance_s(899);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    clk.advance_s(1);
    http.push_ok(fakes::programs_page(pages[2], sizeof(pages[2]), "A", 0, 1, 0, kProgramName),
                 "&skip=0&");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_NOT_NULL(client.program());
}

static void test_failure_retries_same_page() {
    http.push_ok(fakes::programs_page(pages[0], sizeof(pages[0]), "A", 0, 50), "&skip=0&");
    http.push_status(503);
    http.push_ok(fakes::programs_page(pages[1], sizeof(pages[1]), "A", 50, 1, 0, kProgramName),
                 "&skip=50&");
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    client.loop();
    ASSERT_STATUS(VenStatus::kHttpError, client.status().status);
    TEST_ASSERT_TRUE(strstr(http.last_url(), "&skip=50&") != nullptr);
    TEST_ASSERT_EQUAL_UINT32(10000, client.ms_until_due());
    clk.advance_s(10);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_NOT_NULL(client.program());
    ASSERT_STATUS(VenStatus::kResolvingProgram, client.status().status);
    TEST_ASSERT_EQUAL_UINT16(0, client.status().consecutive_failures);
}

static void test_page_cap() {
    http.push_ok(fakes::programs_page(pages[0], sizeof(pages[0]), "A", 0, 50), nullptr,
                 OA3_PROGRAM_PAGES_MAX);
    TEST_ASSERT_TRUE(client.start(config()));
    for (unsigned i = 0; i < OA3_PROGRAM_PAGES_MAX; ++i) {
        ASSERT_STATUS(VenStatus::kResolvingProgram, client.status().status);
        client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(OA3_PROGRAM_PAGES_MAX, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    char expect[32];
    snprintf(expect, sizeof(expect), "&skip=%u&", (OA3_PROGRAM_PAGES_MAX - 1) * 50);
    TEST_ASSERT_TRUE(strstr(http.last_url(), expect) != nullptr);
    ASSERT_STATUS(VenStatus::kProgramNotFound, client.status().status);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(OA3_PROGRAM_PAGES_MAX, http.requests);
    clk.advance_s(900);
    http.push_ok("[]", "&skip=0&");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    ASSERT_STATUS(VenStatus::kProgramNotFound, client.status().status);
}

static void test_by_id() {
    oa3::VenConfig c = config();
    c.program_id = kProgramId;
    http.push_ok(program_object, "/openadr3/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc");
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_NOT_NULL(client.program());
    TEST_ASSERT_EQUAL_STRING(kProgramName, client.program()->program_name);

    c.program_name = nullptr;
    http.push_ok(program_object, "/programs/b54d3d1f");
    TEST_ASSERT_TRUE(client.start(c));
    TEST_ASSERT_NULL(client.program());
    client.loop();
    TEST_ASSERT_NOT_NULL(client.program());
}

static void test_by_id_not_found() {
    oa3::VenConfig c = config();
    c.program_id = "missing";
    http.push_status(404, R"({"title":"Not Found","status":404})");
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    ASSERT_STATUS(VenStatus::kProgramNotFound, client.status().status);
    TEST_ASSERT_EQUAL_INT(404, client.status().http_status);
    TEST_ASSERT_EQUAL_UINT32(900000, client.ms_until_due());
    TEST_ASSERT_NULL(client.program());
    clk.advance_s(900);
    http.push_status(404);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
}

static void test_by_id_name_mismatch() {
    oa3::VenConfig c = config();
    c.program_id = kProgramId;
    c.program_name = "EELEC-012041131";
    http.push_ok(program_object);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    ASSERT_STATUS(VenStatus::kProgramNotFound, client.status().status);
    TEST_ASSERT_NULL(client.program());
    TEST_ASSERT_EQUAL_UINT32(900000, client.ms_until_due());
}

static void test_send_program_name_false() {
    oa3::VenConfig c = config();
    c.send_program_name = false;
    http.push_ok(programs_fixture);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    TEST_ASSERT_EQUAL_STRING(
        "https://price.grid-coordination.energy/openadr3/3.1.0/programs?skip=0&limit=50",
        http.url(0));
    TEST_ASSERT_NOT_NULL(client.program());
}

static void test_targets_sent() {
    oa3::VenConfig c = config();
    const char* targets[] = {"T 1", "T2"};
    c.targets = targets;
    c.target_count = 2;
    http.push_ok(programs_fixture);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    TEST_ASSERT_EQUAL_STRING(
        "https://price.grid-coordination.energy/openadr3/3.1.0/programs?programName=EELEC-013532223"
        "&targets=T%201&targets=T2&skip=0&limit=50",
        http.url(0));
}


static void test_program_name_refused_falls_back_to_paging() {
    http.push(fakes::Response{"/programs?programName=", oa3::HttpResult::kOk, 400, "", true, 1});
    http.push_ok(fakes::programs_page(pages[0], sizeof(pages[0]), "A", 0, 50), "/programs?skip=0&limit=50");
    http.push_ok(fakes::programs_page(pages[1], sizeof(pages[1]), "A", 50, 3, 1, kProgramName),
                 "/programs?skip=50&limit=50");
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    TEST_ASSERT_NULL(client.program());
    TEST_ASSERT_EQUAL_UINT32(0, client.ms_until_due());
    TEST_ASSERT_EQUAL_UINT16(0, client.status().consecutive_failures);
    client.loop();
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_NOT_NULL(client.program());
    TEST_ASSERT_EQUAL_STRING(kProgramId, client.program()->id);
}

static void test_server_error_on_program_name_is_not_refusal() {
    http.push(fakes::Response{"/programs?programName=", oa3::HttpResult::kOk, 503, "", true, 1});
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    ASSERT_STATUS(VenStatus::kHttpError, client.status().status);
    clk.advance_s(10);
    http.push_ok(programs_fixture, "/programs?programName=");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_NOT_NULL(client.program());
}

static void test_program_id_not_url_safe() {
    static const char kBadId[] =
        R"([{"id":"a b/c","programName":"EELEC-013532223","objectType":"PROGRAM"}])";
    http.push_ok(kBadId);
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    TEST_ASSERT_NULL(client.program());
    ASSERT_STATUS(VenStatus::kParseError, client.status().status);
    TEST_ASSERT_EQUAL_STRING("INVALID_FIELD", oa3::to_string(client.status().parse_result));
}

int main() {
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_programs.json", programs_fixture,
                     sizeof(programs_fixture));
    size_t n = strlen(programs_fixture);
    while (n > 0 && programs_fixture[n - 1] != ']') {
        --n;
    }
    memcpy(program_object, programs_fixture + 1, n - 2);
    program_object[n - 2] = '\0';

    UNITY_BEGIN();
    RUN_TEST(test_server_honors_program_name);
    RUN_TEST(test_server_ignores_program_name);
    RUN_TEST(test_found_on_page_two);
    RUN_TEST(test_not_found_then_retry_after_poll_period);
    RUN_TEST(test_failure_retries_same_page);
    RUN_TEST(test_page_cap);
    RUN_TEST(test_by_id);
    RUN_TEST(test_by_id_not_found);
    RUN_TEST(test_by_id_name_mismatch);
    RUN_TEST(test_send_program_name_false);
    RUN_TEST(test_targets_sent);
    RUN_TEST(test_program_name_refused_falls_back_to_paging);
    RUN_TEST(test_server_error_on_program_name_is_not_refusal);
    RUN_TEST(test_program_id_not_url_safe);
    return UNITY_END();
}
