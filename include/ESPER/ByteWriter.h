#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace esper {

// Growable little-endian sequential writer — the emit-side mirror of
// ByteReader. Supports offset patch-back for size fields that are only known
// after their content is written (GRUP groupSize, XXXX extension sizes).
struct ByteWriter {
    std::vector<uint8_t> buf;

    size_t Tell() const { return buf.size(); }

    void U8(uint8_t v)  { buf.push_back(v); }
    void U16(uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void U32(uint32_t v) {
        for (int i = 0; i < 4; ++i)
            buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void U64(uint64_t v) {
        for (int i = 0; i < 8; ++i)
            buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void F32(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        U32(bits);
    }
    void Bytes(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), p, p + n);
    }
    // Writes the string's characters plus a terminating NUL.
    void ZString(const std::string& s) {
        Bytes(s.data(), s.size());
        buf.push_back(0);
    }

    void PatchU32(size_t offset, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            buf[offset + static_cast<size_t>(i)] =
                static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    void PatchU16(size_t offset, uint16_t v) {
        buf[offset]     = static_cast<uint8_t>(v & 0xFF);
        buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
};

} // namespace esper
