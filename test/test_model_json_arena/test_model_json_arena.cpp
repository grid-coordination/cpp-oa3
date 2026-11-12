#include <stdint.h>
#include <string.h>

#include <ArduinoJson.h>
#include <oa3/model/json_arena.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

// Each block is preceded by a header of two size_t.
static const size_t kHeader = 2 * sizeof(size_t);

static bool aligned(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 7) == 0; }

static void test_fill_until_exhausted() {
    oa3::StaticJsonArena<256> arena;
    TEST_ASSERT_EQUAL_size_t(256, arena.capacity());
    size_t blocks = 0;
    for (;;) {
        void* p = arena.allocate(20);
        if (p == nullptr) {
            break;
        }
        TEST_ASSERT_TRUE(aligned(p));
        ++blocks;
    }
    TEST_ASSERT_EQUAL_size_t(256 / (kHeader + 24), blocks);
    TEST_ASSERT_TRUE(arena.exhausted());
    TEST_ASSERT_EQUAL_size_t(blocks * (kHeader + 24), arena.used());
    TEST_ASSERT_EQUAL_size_t(arena.used(), arena.high_water());
    TEST_ASSERT_NULL(arena.allocate(256 - arena.used()));
}

static void test_deallocate_pops_newest_only() {
    oa3::StaticJsonArena<256> arena;
    void* a = arena.allocate(8);
    void* b = arena.allocate(8);
    TEST_ASSERT_EQUAL_size_t(2 * (kHeader + 8), arena.used());
    arena.deallocate(a);
    TEST_ASSERT_EQUAL_size_t(2 * (kHeader + 8), arena.used());
    arena.deallocate(b);
    TEST_ASSERT_EQUAL_size_t(kHeader + 8, arena.used());
    arena.deallocate(a);
    TEST_ASSERT_EQUAL_size_t(0, arena.used());
    arena.deallocate(nullptr);
    TEST_ASSERT_EQUAL_size_t(2 * (kHeader + 8), arena.high_water());
}

static void test_reallocate_in_place() {
    oa3::StaticJsonArena<256> arena;
    arena.allocate(8);
    char* p = static_cast<char*>(arena.allocate(16));
    memcpy(p, "0123456789abcdef", 16);
    char* q = static_cast<char*>(arena.reallocate(p, 100));
    TEST_ASSERT_EQUAL_PTR(p, q);
    TEST_ASSERT_EQUAL_size_t(2 * kHeader + 8 + 104, arena.used());
    TEST_ASSERT_EQUAL_MEMORY("0123456789abcdef", q, 16);
    q = static_cast<char*>(arena.reallocate(q, 4));
    TEST_ASSERT_EQUAL_PTR(p, q);
    TEST_ASSERT_EQUAL_size_t(2 * kHeader + 8 + 8, arena.used());
    TEST_ASSERT_EQUAL_size_t(2 * kHeader + 8 + 104, arena.high_water());
    TEST_ASSERT_NULL(arena.reallocate(q, 1000));
    TEST_ASSERT_TRUE(arena.exhausted());
}

static void test_reallocate_by_copy() {
    oa3::StaticJsonArena<256> arena;
    char* a = static_cast<char*>(arena.allocate(16));
    memcpy(a, "0123456789abcdef", 16);
    arena.allocate(8);
    char* b = static_cast<char*>(arena.reallocate(a, 32));
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(b != a);
    TEST_ASSERT_TRUE(aligned(b));
    TEST_ASSERT_EQUAL_MEMORY("0123456789abcdef", b, 16);
    char* c = static_cast<char*>(arena.reallocate(a, 4));
    TEST_ASSERT_EQUAL_MEMORY("0123", c, 4);
    void* d = arena.reallocate(nullptr, 8);
    TEST_ASSERT_NOT_NULL(d);
}

static void test_reset() {
    oa3::StaticJsonArena<64> arena;
    TEST_ASSERT_NOT_NULL(arena.allocate(40));
    TEST_ASSERT_NULL(arena.allocate(40));
    TEST_ASSERT_TRUE(arena.exhausted());
    arena.reset();
    TEST_ASSERT_FALSE(arena.exhausted());
    TEST_ASSERT_EQUAL_size_t(0, arena.used());
    TEST_ASSERT_EQUAL_size_t(kHeader + 40, arena.high_water());
    TEST_ASSERT_NOT_NULL(arena.allocate(40));
}

static void test_unaligned_buffer() {
    alignas(8) static unsigned char storage[72];
    oa3::JsonArena arena(storage + 3, 69);
    TEST_ASSERT_EQUAL_size_t(64, arena.capacity());
    void* p = arena.allocate(1);
    TEST_ASSERT_TRUE(aligned(p));
    TEST_ASSERT_TRUE(static_cast<unsigned char*>(p) >= storage + 3);
}

static void test_json_document() {
    static oa3::StaticJsonArena<8192> arena;
    const char* json = R"({"a":[1,2,3],"b":"a string that is longer than thirty-two bytes","c":{"d":true}})";
    {
        ArduinoJson::JsonDocument doc(&arena);
        TEST_ASSERT_FALSE(deserializeJson(doc, json));
        TEST_ASSERT_EQUAL_INT(3, doc["a"][2].as<int>());
        TEST_ASSERT_EQUAL_STRING("a string that is longer than thirty-two bytes", doc["b"].as<const char*>());
        TEST_ASSERT_TRUE(doc["c"]["d"].as<bool>());
        TEST_ASSERT_TRUE(arena.used() > 0);
    }
    TEST_ASSERT_FALSE(arena.exhausted());
    arena.reset();

    oa3::StaticJsonArena<64> tiny;
    ArduinoJson::JsonDocument doc(&tiny);
    TEST_ASSERT_TRUE(deserializeJson(doc, json) == ArduinoJson::DeserializationError::NoMemory);
    TEST_ASSERT_TRUE(tiny.exhausted());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fill_until_exhausted);
    RUN_TEST(test_deallocate_pops_newest_only);
    RUN_TEST(test_reallocate_in_place);
    RUN_TEST(test_reallocate_by_copy);
    RUN_TEST(test_reset);
    RUN_TEST(test_unaligned_buffer);
    RUN_TEST(test_json_document);
    return UNITY_END();
}
