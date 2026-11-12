#pragma once

#include <stddef.h>
#include <string.h>

namespace oa3 {

// A byte source with the two members ArduinoJson's deserializeJson() reads through. A port
// wraps its HTTP body or MQTT payload in one.
class ByteReader {
public:
    // The next byte as 0..255, or -1 at the end of input.
    virtual int read() = 0;
    // Up to length bytes; fewer only at the end of input.
    virtual size_t readBytes(char* buffer, size_t length) = 0;

protected:
    ~ByteReader() = default;
};

class BufferReader final : public ByteReader {
public:
    BufferReader(const char* data, size_t len) : _data(data), _len(len), _pos(0) {}

    int read() override {
        return _pos < _len ? static_cast<unsigned char>(_data[_pos++]) : -1;
    }

    size_t readBytes(char* buffer, size_t length) override {
        size_t n = _len - _pos < length ? _len - _pos : length;
        memcpy(buffer, _data + _pos, n);
        _pos += n;
        return n;
    }

private:
    const char* _data;
    size_t _len;
    size_t _pos;
};

}  // namespace oa3
