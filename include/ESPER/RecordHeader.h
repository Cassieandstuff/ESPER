#pragma once
#include <cstdint>

namespace esper {

// Binary record-stream layout (packed). Bethesda's plugin format is a flat
// stream of records and GRUP groups; all multi-byte fields are little-endian.

#pragma pack(push, 1)

// Top-level record header (TES4, NPC_, RACE, ...).
struct RecHeader {
    uint32_t sig;
    uint32_t dataSize;   // bytes of record data after this header
    uint32_t flags;
    uint32_t formId;
    uint16_t timestamp;
    uint16_t vcInfo;
    uint16_t version;
    uint16_t unknown;
};
static_assert(sizeof(RecHeader) == 24);

// Group header. groupSize INCLUDES this 24-byte header.
struct GrupHeader {
    uint32_t sig;
    uint32_t groupSize;
    uint32_t label;
    int32_t  groupType;
    uint16_t timestamp;
    uint16_t vcInfo;
    uint32_t unknown;
};
static_assert(sizeof(GrupHeader) == 24);

// Subrecord header.
struct SubHeader {
    uint32_t sig;
    uint16_t dataSize;
};
static_assert(sizeof(SubHeader) == 6);

#pragma pack(pop)

} // namespace esper
