#include <ESPER/GroupTree.h>

#include <ESPER/Zlib.h>

#include <cstdio>
#include <cstring>

namespace esper {

// `raw` and `modified` both hold the ON-DISK form of the payload (compressed
// records: [u32 decompressedSize][zlib stream]); `inflated` caches the logical
// bytes for compressed records. Payload() always returns the LOGICAL payload.

std::span<const uint8_t> RecordNode::Payload(char* err, int errLen) {
    const uint8_t* disk     = modified ? modified->data() : raw;
    const uint32_t diskSize = modified ? static_cast<uint32_t>(modified->size())
                                       : rawSize;

    if (!Compressed()) {
        // Empty-but-valid: hand back a non-null pointer so callers can tell
        // "no data" from "decompression failed".
        static const uint8_t kEmpty = 0;
        if (diskSize == 0) return { &kEmpty, size_t{0} };
        return { disk, diskSize };
    }

    if (inflated)
        return { inflated->data(), inflated->size() };

    if (diskSize < 4 || disk == nullptr) {
        if (err && errLen > 0)
            std::snprintf(err, static_cast<size_t>(errLen),
                          "compressed record %08X too small (%u bytes)",
                          header.formId, diskSize);
        return {};
    }

    uint32_t decompressedSize = 0;
    std::memcpy(&decompressedSize, disk, 4);

    auto out = std::make_unique<std::vector<uint8_t>>();
    if (!ZlibInflate(disk + 4, static_cast<size_t>(diskSize) - 4u,
                     decompressedSize, *out, err, errLen))
        return {};

    inflated = std::move(out);
    return { inflated->data(), inflated->size() };
}

} // namespace esper
