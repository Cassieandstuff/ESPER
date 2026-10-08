#pragma once
#include <cstdint>
#include <cstring>

namespace esper {

// Bounds-checked little-endian sequential reader over a [p, end) byte range.
// Every read that would exceed `end` sets ok=false and returns a zero value.
struct ByteReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok;

    ByteReader(const uint8_t* start, const uint8_t* e)
        : p(start), end(e), ok(start <= e) {}

    float F32() {
        if (p + 4 > end) { ok = false; return 0.f; }
        float v; std::memcpy(&v, p, 4); p += 4; return v;
    }
    uint32_t U32() {
        if (p + 4 > end) { ok = false; return 0; }
        uint32_t v; std::memcpy(&v, p, 4); p += 4; return v;
    }
    uint16_t U16() {
        if (p + 2 > end) { ok = false; return 0; }
        uint16_t v; std::memcpy(&v, p, 2); p += 2; return v;
    }
    uint8_t U8() {
        if (p >= end) { ok = false; return 0; }
        return *p++;
    }

    const uint8_t* Bytes(int n) {
        if (n < 0 || p + n > end) { ok = false; return nullptr; }
        const uint8_t* r = p;
        p += n;
        return r;
    }

    void skip(int n) {
        if (n < 0 || p + n > end) { ok = false; p = end; return; }
        p += n;
    }

    void align4(const uint8_t* base) {
        uintptr_t off          = static_cast<uintptr_t>(p - base);
        const uint8_t* aligned = base + ((off + 3u) & ~static_cast<uintptr_t>(3u));
        if (aligned > end) { ok = false; p = end; return; }
        p = aligned;
    }

    size_t Remaining() const {
        return (p < end) ? static_cast<size_t>(end - p) : 0u;
    }
};

} // namespace esper
