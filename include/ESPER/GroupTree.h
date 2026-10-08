#pragma once
#include <ESPER/PluginFlags.h>
#include <ESPER/RecordHeader.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace esper {

// GRUP groupType values (GrupHeader::groupType). The label field's meaning
// depends on the type: a record signature for top groups, a FormID for
// world/cell/topic children, block/sub-block numbers or packed cell coords for
// the spatial partition groups.
namespace group_type {
    constexpr int32_t kTop                = 0;  // label = contained record sig
    constexpr int32_t kWorldChildren      = 1;  // label = WRLD formId
    constexpr int32_t kInteriorCellBlock  = 2;  // label = block number
    constexpr int32_t kInteriorCellSub    = 3;  // label = sub-block number
    constexpr int32_t kExteriorCellBlock  = 4;  // label = (blockY<<16)|blockX (int16 pair)
    constexpr int32_t kExteriorCellSub    = 5;  // label = (subY<<16)|subX (int16 pair)
    constexpr int32_t kCellChildren       = 6;  // label = CELL formId
    constexpr int32_t kTopicChildren      = 7;  // label = DIAL formId
    constexpr int32_t kCellPersistent     = 8;  // label = CELL formId
    constexpr int32_t kCellTemporary      = 9;  // label = CELL formId
    constexpr int32_t kCellVisibleDistant = 10; // pre-SSE only; retained for tolerance
}

// A parse/emit diagnostic tied to a byte offset in the source file. Malformed
// structure is recorded here and skipped — it never aborts the walk.
struct Diagnostic {
    std::string message;
    uint64_t    offset = 0;
};

// One record in the tree. `raw` points INTO the owning Plugin's buffer (as on
// disk — possibly zlib-compressed with a leading uint32 decompressed size).
// Payload() is the one accessor consumers use: it returns the record's logical
// (decompressed, current) data, lazily inflating compressed records once.
// Not thread-safe: the lazy inflate mutates the node.
struct RecordNode {
    RecHeader      header{};        // full 24 bytes preserved for round-trip
    const uint8_t* raw     = nullptr;
    uint32_t       rawSize = 0;

    // Lazy decompression cache (only allocated for compressed records).
    std::unique_ptr<std::vector<uint8_t>> inflated;
    // Replacement payload set by the mutation API; overrides raw on save.
    std::unique_ptr<std::vector<uint8_t>> modified;
    bool dirty = false;

    bool Compressed() const { return (header.flags & rec_flag::kCompressed) != 0; }
    bool Deleted() const    { return (header.flags & rec_flag::kDeleted)    != 0; }

    // modified > inflated > raw. Returns an empty span with data()==nullptr on
    // decompression failure (err filled); a genuinely empty record returns an
    // empty span over a non-null pointer.
    std::span<const uint8_t> Payload(char* err, int errLen);
};

struct GroupNode;

// Tree node: either a record or (when `group` is non-null) a nested group.
// Records and groups interleave at the same level (e.g. a CELL record is
// followed by its cell-children GRUP as a sibling).
struct Node {
    RecordNode                 rec;
    std::unique_ptr<GroupNode> group;

    bool IsGroup() const { return group != nullptr; }
};

struct GroupNode {
    GrupHeader        header{};     // label/type preserved; groupSize recomputed on save
    std::vector<Node> children;
};

} // namespace esper
