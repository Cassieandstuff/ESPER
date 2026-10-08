#pragma once
#include <ESPER/Schema.h>

#include <cstdint>

namespace esper {

// Shared typed pieces used across many record schemas. Layouts are the
// documented SSE forms (UESP); any on-disk size variant a codec doesn't match
// routes to unknown-passthrough, so fidelity never depends on completeness.

#pragma pack(push, 1)

// OBND — object bounds, 12 bytes.
struct ObjectBounds {
    int16_t x1 = 0, y1 = 0, z1 = 0;
    int16_t x2 = 0, y2 = 0, z2 = 0;
};
static_assert(sizeof(ObjectBounds) == 12);

// NPC_ ACBS — base stats, 24 bytes.
struct AcbsData {
    uint32_t flags = 0;              // bit 0 = female
    uint16_t magickaOffset = 0;
    uint16_t staminaOffset = 0;
    uint16_t level = 0;              // or level ×1000 when PC-level-mult
    uint16_t calcMin = 0;
    uint16_t calcMax = 0;
    uint16_t speedMultiplier = 0;
    uint16_t dispositionBase = 0;
    uint16_t templateFlags = 0;
    uint16_t healthOffset = 0;
    uint16_t bleedoutOverride = 0;
};
static_assert(sizeof(AcbsData) == 24);

// NPC_ SNAM — faction membership, 8 bytes.
struct FactionRank {
    uint32_t faction = 0;            // FormID
    uint8_t  rank    = 0;
    uint8_t  pad[3]  = { 0, 0, 0 };
};
static_assert(sizeof(FactionRank) == 8);
inline constexpr FieldDef kFactionRankIds[] = { FormIdAt(0) };

// NPC_ AIDT — AI data, 20 bytes.
struct AiData {
    uint8_t  aggression = 0;
    uint8_t  confidence = 0;
    uint8_t  energy = 0;
    uint8_t  morality = 0;
    uint8_t  mood = 0;
    uint8_t  assistance = 0;
    uint8_t  flags = 0;
    uint8_t  pad = 0;
    uint32_t warn = 0;
    uint32_t warnAttack = 0;
    uint32_t attack = 0;
};
static_assert(sizeof(AiData) == 20);

// CNTO — container/inventory entry, 8 bytes.
struct ContainerEntry {
    uint32_t item  = 0;              // FormID
    int32_t  count = 0;
};
static_assert(sizeof(ContainerEntry) == 8);
inline constexpr FieldDef kContainerEntryIds[] = { FormIdAt(0) };

// BOD2 — biped body template, 8 bytes (SSE form).
struct BipedBodyTemplate {
    uint32_t bipedFlags = 0;         // slot mask (bit2 = body, bit3 = hands, bit7 = feet)
    uint32_t armorType  = 0;         // 0 light, 1 heavy, 2 clothing
};
static_assert(sizeof(BipedBodyTemplate) == 8);

// ARMO DATA — value/weight, 8 bytes.
struct ItemValueWeight {
    int32_t value  = 0;
    float   weight = 0.f;
};
static_assert(sizeof(ItemValueWeight) == 8);

// DODT — decal data, 36 bytes (TXST).
struct DecalData {
    float    minWidth = 0.f, maxWidth = 0.f;
    float    minHeight = 0.f, maxHeight = 0.f;
    float    depth = 0.f, shininess = 0.f;
    float    parallaxScale = 0.f;
    uint8_t  parallaxPasses = 0;
    uint8_t  flags = 0;
    uint16_t pad = 0;
    uint32_t color = 0;
};
static_assert(sizeof(DecalData) == 36);

#pragma pack(pop)

// KWDA-style payload: a whole array of FormIDs inside ONE subrecord.
struct FormRefArray {
    static bool Decode(std::vector<FormRef>& out, SubView s, const SchemaCtx&) {
        if (s.size % 4 != 0) return false;
        const uint32_t n = s.size / 4u;
        out.resize(n);
        for (uint32_t i = 0; i < n; ++i)
            std::memcpy(&out[i].id, s.data + i * 4u, 4);
        return true;
    }
    static void Encode(const std::vector<FormRef>& v, const SchemaCtx&, ByteWriter& w) {
        for (const FormRef& r : v) w.U32(r.id);
    }
    static void VisitIds(std::vector<FormRef>& v,
                         void (*fn)(uint32_t&, void*), void* ctx) {
        for (FormRef& r : v) fn(r.id, ctx);
    }
};

} // namespace esper
