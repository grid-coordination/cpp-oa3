#pragma once

#include <stddef.h>

#include <ArduinoJson.h>

#include <oa3/model/limits.h>

namespace oa3 {

// A bump allocator over caller-owned memory for a JsonDocument, so parsing never reaches
// the heap. reset() before each document. reallocate() grows or shrinks the newest block
// in place, and otherwise copies into a new block.
class JsonArena : public ArduinoJson::Allocator {
public:
    JsonArena(void* buffer, size_t size);

    void reset();
    size_t capacity() const { return _size; }
    size_t used() const { return _used; }
    size_t high_water() const { return _high_water; }
    // An allocation has failed since the last reset().
    bool exhausted() const { return _exhausted; }

    void* allocate(size_t size) override;
    void deallocate(void* ptr) override;
    void* reallocate(void* ptr, size_t new_size) override;

private:
    unsigned char* _buffer;
    size_t _size;
    size_t _used;
    size_t _high_water;
    size_t _last;  // offset of the newest block's header, or _size when none
    bool _exhausted;
};

template <size_t N = OA3_JSON_ARENA_SIZE>
class StaticJsonArena : public JsonArena {
public:
    StaticJsonArena() : JsonArena(_storage, N) {}

private:
    alignas(8) unsigned char _storage[N];
};

}  // namespace oa3
