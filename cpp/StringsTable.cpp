#include <ESPER/StringsTable.h>

#include <ESPER/ByteReader.h>

#include <cstring>

namespace esper {

namespace {
struct DirEntry {
    uint32_t stringId;
    uint32_t offset;
};
} // namespace

bool StringsTable::Parse(const uint8_t* data, size_t size, bool lengthPrefixed) {
    map_.clear();
    if (!data) return false;

    ByteReader r(data, data + size);

    const uint32_t count    = r.U32();
    const uint32_t dataSize = r.U32();
    if (!r.ok) return false;

    std::vector<DirEntry> dir;
    dir.reserve(count < 65536u ? count : 65536u);
    for (uint32_t i = 0; i < count; ++i) {
        DirEntry e{};
        e.stringId = r.U32();
        e.offset   = r.U32();
        if (!r.ok) break;
        dir.push_back(e);
    }

    const uint8_t* blockStart = r.p;
    const size_t   avail      = (blockStart <= r.end)
                                    ? static_cast<size_t>(r.end - blockStart)
                                    : 0u;
    const size_t   blockLen   = (dataSize < avail) ? dataSize : avail;
    const uint8_t* blockEnd   = blockStart + blockLen;

    for (const DirEntry& e : dir) {
        if (e.offset >= blockLen) continue;
        const uint8_t* sp = blockStart + e.offset;

        if (lengthPrefixed) {
            if (sp + 4 > blockEnd) continue;
            uint32_t len = 0;
            std::memcpy(&len, sp, 4);
            const uint8_t* strBytes = sp + 4;
            if (len == 0) { map_[e.stringId] = std::string(); continue; }
            if (strBytes + len > blockEnd) continue;
            size_t n = len;
            if (n > 0 && strBytes[n - 1] == 0) --n;
            map_[e.stringId] =
                std::string(reinterpret_cast<const char*>(strBytes), n);
        } else {
            size_t n = 0;
            while (sp + n < blockEnd && sp[n] != 0) ++n;
            map_[e.stringId] =
                std::string(reinterpret_cast<const char*>(sp), n);
        }
    }

    return true;
}

} // namespace esper
