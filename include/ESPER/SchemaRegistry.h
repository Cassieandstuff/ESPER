#pragma once
#include <ESPER/Schema.h>

#include <cstdint>
#include <span>
#include <string>

namespace esper {

// Type-erased handle to one record type's schema, for generic drivers (the
// per-wave decode→encode gate, coverage stats). Typed consumers use the
// schemas directly (kNpcSchema etc. in the Records*.h family headers).
struct SchemaOps {
    uint32_t sig = 0;

    // Decode `payload`, re-encode, byte-compare. Returns false with `why` on
    // any mismatch. `totalSubs`/`unknownSubs` report modeling coverage.
    bool (*roundtrip)(std::span<const uint8_t> payload, const SchemaCtx& ctx,
                      size_t& totalSubs, size_t& unknownSubs,
                      std::string& why) = nullptr;

    // Decode `payload`, apply `fn` to every FormID the schema can see, and
    // re-encode into `outBytes`. REFUSES (false + why) when the payload has
    // any unknown-passthrough subrecords — they could carry FormIDs the
    // visitor can't reach, and a partial remap would corrupt references.
    bool (*remapIds)(std::span<const uint8_t> payload, const SchemaCtx& ctx,
                     void (*fn)(uint32_t& id, void* userCtx), void* userCtx,
                     std::vector<uint8_t>& outBytes, std::string& why) = nullptr;
};

// All registered record schemas (wave coverage grows over time).
std::span<const SchemaOps> AllSchemas();

// nullptr when the sig has no schema yet.
const SchemaOps* FindSchema(uint32_t sig);

} // namespace esper
