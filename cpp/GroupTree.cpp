#include <ESPER/GroupTree.h>

#include <ESPER/Zlib.h>

#include <cstring>

namespace esper {

// `raw` and `modified` both hold the ON-DISK form of the payload (compressed
// records: [u32 decompressedSize][zlib stream]); `inflated` caches the logical
// bytes for compressed records. Payload() always returns the LOGICAL payload.

Result<std::span<const uint8_t>> RecordNode::Payload() {
    const uint8_t* disk     = modified ? modified->data() : raw;
    const uint32_t diskSize = modified ? static_cast<uint32_t>(modified->size())
                                       : rawSize;

    if (!Compressed())
        return std::span<const uint8_t>{ disk, diskSize };

    if (inflated)
        return std::span<const uint8_t>{ inflated->data(), inflated->size() };

    if (diskSize < 4 || disk == nullptr)
        return Fail("compressed record {:08X} too small ({} bytes)", header.formId, diskSize);

    uint32_t decompressedSize = 0;
    std::memcpy(&decompressedSize, disk, 4);

    auto out = ZlibInflate({ disk + 4, static_cast<size_t>(diskSize) - 4u }, decompressedSize);
    if (!out) return Wrap(out.error(), "record {:08X}", header.formId);

    inflated = std::make_unique<std::vector<uint8_t>>(std::move(*out));
    return std::span<const uint8_t>{ inflated->data(), inflated->size() };
}

} // namespace esper
