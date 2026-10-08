#pragma once
#include <ESPER/CtdaFunctions.h>
#include <ESPER/Schema.h>

#include <cstdint>

namespace esper {

// CTDA — a condition entry (fixed 32-byte layout, documented; UESP
// "Mod File Format/CTDA"). CIS1/CIS2 string parameters follow their CTDA as
// separate subrecords; `layout` preserves the pairing positionally.
//
// REMAP SAFETY (why condition rows are bound RemapOpaque): whether param1 /
// param2 / the comparison value hold a FormID depends on `functionIndex`
// (~700 functions) and on the parameters-use-aliases / use-pack-data flags. A
// wrong classification would either corrupt an integer that looks like a
// self-index FormID or miss a real FormID during master-list renumbering —
// both silent. Until a verified per-function parameter-kind table lands, any
// record carrying conditions refuses FormID remapping outright; decode/encode
// remain fully typed and byte-faithful.

#pragma pack(push, 1)
struct CondData {
    uint8_t  op = 0;              // bits 5-7 compare op; bit0 OR, bit2 use-global,
                                  // bit1 params-use-aliases, bit3 use-pack-data
    uint8_t  unknown[3] = {};
    uint32_t comparisonRaw = 0;   // f32 bits, or a GLOB FormID under use-global
    uint16_t functionIndex = 0;
    uint8_t  pad[2] = {};
    uint32_t param1 = 0;
    uint32_t param2 = 0;
    uint32_t runOnType = 0;       // 0 subject 1 target 2 reference 3 combat tgt …
    uint32_t reference = 0;       // FormID when runOnType selects a reference
    int32_t  param3 = -1;

    float ComparisonValue() const {
        float f = 0.f;
        std::memcpy(&f, &comparisonRaw, 4);
        return f;
    }
    bool UsesGlobal() const { return (op & 0x04u) != 0; }
    // When set, form-typed params are quest-alias indices (ints), NOT FormIDs.
    bool ParamsUseAliases() const { return (op & 0x02u) != 0; }
    uint8_t CompareOp() const { return static_cast<uint8_t>(op >> 5); }
    bool IsOr() const { return (op & 0x01u) != 0; }

    // runOnType == 2 (Reference) puts a ref FormID in `reference`.
    static constexpr uint32_t kRunOnReference = 2;
};
static_assert(sizeof(CondData) == 32);
#pragma pack(pop)

// Convenience member bundle for record structs that carry conditions.
//   struct XRec : RecordExtras { ...; ConditionList conds; };
// and in the schema table:
//   ConditionRows<&XRec::conds>()  →  three rows (CTDA, CIS1, CIS2).
struct ConditionList {
    std::vector<CondData>    conditions;   // CTDA
    std::vector<std::string> stringParam1; // CIS1
    std::vector<std::string> stringParam2; // CIS2
};

// ── FormID visitation for conditions (master-insert remapping) ────────────────
// Visits every FormID a condition CAN carry, and only those:
//   • `reference`         when runOnType selects a Reference (2)
//   • the comparison value when Use-Global is set (it is a GLOB FormID)
//   • param1 / param2     when the function table classifies them as Form AND
//                         the params are NOT alias indices
// Anything the function table can't classify is left untouched — the caller
// gates on ConditionsRemapSafe() so an unknown function refuses the whole remap
// rather than silently skipping a real reference.
inline void VisitConditionIds(CondData& c,
                              void (*fn)(uint32_t& id, void* ctx), void* ctx) {
    if (c.runOnType == CondData::kRunOnReference && c.reference != 0)
        fn(c.reference, ctx);
    if (c.UsesGlobal() && c.comparisonRaw != 0)
        fn(c.comparisonRaw, ctx);
    if (!c.ParamsUseAliases()) {
        CtdaFuncInfo info;
        if (LookupCtdaFunc(c.functionIndex, info)) {
            if (info.p1 == CtdaParam::Form && c.param1 != 0) fn(c.param1, ctx);
            if (info.p2 == CtdaParam::Form && c.param2 != 0) fn(c.param2, ctx);
        }
    }
}

// A condition is safely remappable exactly when its function is in the table:
// only then do we know which params are FormIDs. Unknown → refuse.
inline bool ConditionRemapSafe(const CondData& c) {
    CtdaFuncInfo info;
    return LookupCtdaFunc(c.functionIndex, info);
}

namespace schema_detail {

template <auto MemberPtr, class Elem, std::vector<Elem> ConditionList::* Sub, class Codec>
constexpr auto CondSubRow(const char sig[5]) {
    using Rec = typename MemPtrTraits<decltype(MemberPtr)>::Class;
    SubBinder<Rec> b{};
    b.sig = Sig4(sig);
    b.decode = [](Rec& r, SubView s, const SchemaCtx& ctx) -> DecodeResult {
        Elem v{};
        if (!Codec::Decode(v, s, ctx)) return DecodeResult::Passthrough;
        ((r.*MemberPtr).*Sub).push_back(std::move(v));
        return DecodeResult::Consumed;
    };
    b.count = [](const Rec& r) -> uint32_t {
        return static_cast<uint32_t>(((r.*MemberPtr).*Sub).size());
    };
    b.encode = [](const Rec& r, uint32_t occ, const SchemaCtx& ctx, ByteWriter& w) {
        Codec::Encode(((r.*MemberPtr).*Sub)[occ], ctx, w);
    };
    return b;
}

} // namespace schema_detail

// The CTDA row: typed + byte-faithful, with FormID visitation driven by the
// clean-room condition-function table and a remap-safety gate (unknown function
// → refuse). Not blanket remap-opaque any more — conditions built from known
// functions remap correctly.
template <auto MemberPtr>
constexpr auto CtdaRow() {
    using Rec = typename MemPtrTraits<decltype(MemberPtr)>::Class;
    auto b = schema_detail::CondSubRow<MemberPtr, CondData,
                                       &ConditionList::conditions, Packed<CondData>>("CTDA");
    b.visitIds = [](Rec& r, void (*fn)(uint32_t&, void*), void* ctx) {
        for (CondData& c : (r.*MemberPtr).conditions) VisitConditionIds(c, fn, ctx);
    };
    b.remapSafe = [](const Rec& r) -> bool {
        for (const CondData& c : (r.*MemberPtr).conditions)
            if (!ConditionRemapSafe(c)) return false;
        return true;
    };
    return b;
}

// CIS1/CIS2 string parameters (variable names — never FormIDs).
template <auto MemberPtr>
constexpr auto Cis1Row() {
    return schema_detail::CondSubRow<MemberPtr, std::string,
                                     &ConditionList::stringParam1, ZString>("CIS1");
}
template <auto MemberPtr>
constexpr auto Cis2Row() {
    return schema_detail::CondSubRow<MemberPtr, std::string,
                                     &ConditionList::stringParam2, ZString>("CIS2");
}

} // namespace esper
