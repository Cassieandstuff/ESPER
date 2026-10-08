#pragma once
#include <ESPER/ByteWriter.h>
#include <ESPER/PluginFlags.h>
#include <ESPER/RecordHeader.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace esper {

// A single logical subrecord: signature + payload view. For XXXX-extended
// subrecords the view covers the REAL payload (the XXXX pair is folded away
// by SubIterator) and `viaXxxx` is set so an emitter can reproduce the exact
// on-disk framing.
struct SubView {
    uint32_t       sig  = 0;
    const uint8_t* data = nullptr;
    uint32_t       size = 0;
    bool           viaXxxx = false;

    std::span<const uint8_t> Span() const { return { data, size }; }
};

// Iterates the subrecords of one record payload, folding the XXXX
// size-extension mechanism: a 4-byte `XXXX` subrecord carries a uint32 that
// REPLACES the following subrecord's 16-bit dataSize (used for payloads
// > 0xFFFF bytes, e.g. NAVM geometry and large LAND/VTXT data).
//
//   SubIterator it(payload);
//   SubView sub;
//   while (it.Next(sub)) { switch (sub.sig) { ... } }
//
// A malformed tail (truncated header or payload) ends iteration; `ok()` then
// reports false so callers can distinguish clean exhaustion from truncation.
class SubIterator {
public:
    explicit SubIterator(std::span<const uint8_t> payload)
        : p_(payload.data()), end_(payload.data() + payload.size()) {}

    SubIterator(const uint8_t* data, size_t size)
        : p_(data), end_(data + size) {}

    bool Next(SubView& out) {
        uint32_t extSize    = 0;
        bool     hasExtSize = false;

        for (;;) {
            if (p_ == end_) return false;                    // clean end
            if (p_ + sizeof(SubHeader) > end_) { ok_ = false; return false; }

            SubHeader sh{};
            std::memcpy(&sh, p_, sizeof(sh));
            const uint8_t* payload = p_ + sizeof(SubHeader);

            if (sh.sig == Sig4("XXXX") && sh.dataSize == 4) {
                if (payload + 4 > end_) { ok_ = false; return false; }
                std::memcpy(&extSize, payload, 4);
                hasExtSize = true;
                p_ = payload + 4;
                continue;                                    // fold into next sub
            }

            const uint32_t size = hasExtSize ? extSize : sh.dataSize;
            if (payload + size > end_) { ok_ = false; return false; }

            out.sig     = sh.sig;
            out.data    = payload;
            out.size    = size;
            out.viaXxxx = hasExtSize;
            p_ = payload + size;
            return true;
        }
    }

    // True while no truncated/malformed structure has been hit.
    bool ok() const { return ok_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool           ok_ = true;
};

// Frames one subrecord into a ByteWriter — the emit-side mirror of
// SubIterator. Inserts the XXXX size-extension pair when the payload exceeds
// the 16-bit size field, or when `forceXxxx` reproduces an original XXXX
// framing that a smaller payload happened to use.
inline void EmitSubrecord(ByteWriter& w, uint32_t sig,
                          const void* data, size_t size, bool forceXxxx = false) {
    if (size > 0xFFFFu || forceXxxx) {
        w.U32(Sig4("XXXX"));
        w.U16(4);
        w.U32(static_cast<uint32_t>(size));
        w.U32(sig);
        w.U16(0);
    } else {
        w.U32(sig);
        w.U16(static_cast<uint16_t>(size));
    }
    if (size) w.Bytes(data, size);
}

// Reads a zero-terminated (or length-bounded) string from a subrecord payload.
inline std::string ReadZString(const uint8_t* data, uint32_t len) {
    size_t n = 0;
    while (n < len && data[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(data), n);
}

inline std::string ReadZString(const SubView& sub) {
    return ReadZString(sub.data, sub.size);
}

} // namespace esper
