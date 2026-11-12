#include <oa3/model/json_arena.h>

#include <stdint.h>
#include <string.h>

namespace oa3 {

namespace {

constexpr size_t kAlign = 8;

// Precedes each block. prev is the offset of the previous newest block's header, so
// deallocate() can pop back to it.
struct BlockHeader {
    size_t prev;
    size_t size;
};

static_assert(sizeof(BlockHeader) % kAlign == 0, "a header keeps the next block aligned");

size_t round_up(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }

}  // namespace

JsonArena::JsonArena(void* buffer, size_t size)
    : _buffer(static_cast<unsigned char*>(buffer)), _size(size), _used(0), _high_water(0),
      _last(0), _exhausted(false) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(_buffer);
    size_t skew = (kAlign - (addr & (kAlign - 1))) & (kAlign - 1);
    if (_buffer == nullptr || skew > _size) {
        _size = 0;
    } else {
        _buffer += skew;
        _size -= skew;
    }
    _size &= ~(kAlign - 1);
    _last = _size;
}

void JsonArena::reset() {
    _used = 0;
    _last = _size;
    _exhausted = false;
}

void* JsonArena::allocate(size_t size) {
    size_t rounded = round_up(size);
    if (rounded < size || rounded > _size - _used ||
        sizeof(BlockHeader) > _size - _used - rounded) {
        _exhausted = true;
        return nullptr;
    }
    BlockHeader h;
    h.prev = _last;
    h.size = rounded;
    memcpy(_buffer + _used, &h, sizeof(h));
    _last = _used;
    _used += sizeof(BlockHeader) + rounded;
    if (_used > _high_water) {
        _high_water = _used;
    }
    return _buffer + _last + sizeof(BlockHeader);
}

void JsonArena::deallocate(void* ptr) {
    if (ptr == nullptr || _last == _size || ptr != _buffer + _last + sizeof(BlockHeader)) {
        return;
    }
    BlockHeader h;
    memcpy(&h, _buffer + _last, sizeof(h));
    _used = _last;
    _last = h.prev;
}

void* JsonArena::reallocate(void* ptr, size_t new_size) {
    if (ptr == nullptr) {
        return allocate(new_size);
    }
    unsigned char* p = static_cast<unsigned char*>(ptr);
    size_t rounded = round_up(new_size);
    if (_last != _size && p == _buffer + _last + sizeof(BlockHeader)) {
        size_t data = _last + sizeof(BlockHeader);
        if (rounded < new_size || rounded > _size - data) {
            _exhausted = true;
            return nullptr;
        }
        BlockHeader h;
        memcpy(&h, _buffer + _last, sizeof(h));
        h.size = rounded;
        memcpy(_buffer + _last, &h, sizeof(h));
        _used = data + rounded;
        if (_used > _high_water) {
            _high_water = _used;
        }
        return ptr;
    }
    BlockHeader old;
    memcpy(&old, p - sizeof(BlockHeader), sizeof(old));
    void* q = allocate(new_size);
    if (q != nullptr) {
        memcpy(q, p, old.size < new_size ? old.size : new_size);
    }
    return q;
}

}  // namespace oa3
