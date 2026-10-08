#pragma once
#include <ESPER/ByteWriter.h>
#include <ESPER/Subrecords.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace esper {

// ─────────────────────────────────────────────────────────────────────────────
// The record schema system — the omnidirectional round-trip SOP applied to
// record payloads. ONE declarative table per record type; decode, encode,
// presence and FormID visitation all derive from that table, so the two
// directions cannot drift.
//
// Fidelity model (load-bearing):
//   • Decode walks the subrecord stream. Each subrecord either binds to a
//     table row (typed member set, position recorded in `layout`) or is
//     retained verbatim in `unknown`. A row REFUSES a subrecord whose shape
//     it can't reproduce exactly (odd size, second copy of a singular field)
//     — refusal routes it to `unknown`, so fidelity never depends on schema
//     completeness.
//   • Encode replays `layout` in original order: bound entries re-encode from
//     the typed member, unknown entries emit verbatim (original XXXX framing
//     included). Members present without a layout slot (added after decode /
//     new records) append at the end in schema-table order.
//   • The per-wave gate (decode → encode == original payload bytes, for every
//     instance in the vanilla load order) is the drift alarm.
//
// Field storage conventions: singular fields are std::optional<T> (presence =
// has_value, so a refused/passthrough'd subrecord never double-emits);
// repeating fields are std::vector<T>. Every typed record struct derives from
// RecordExtras, which carries layout + unknown.
// ─────────────────────────────────────────────────────────────────────────────

// Context a codec may need beyond the bytes (e.g. the TES4 kLocalized flag
// changes how string fields are stored).
struct SchemaCtx {
    bool localized = false;
};

enum class DecodeResult : uint8_t {
    Consumed,      // typed member set; row will re-emit it
    Passthrough,   // shape not reproducible from the typed member; keep raw
};

// A subrecord kept verbatim (not semantically modeled, or refused by its row).
struct UnknownSub {
    uint32_t             sig = 0;
    bool                 viaXxxx = false;
    std::vector<uint8_t> bytes;
};

// Base every typed record struct derives from: the original subrecord
// sequence (drives encode order) + the verbatim-retained subrecords.
struct RecordExtras {
    struct LayoutEntry {
        int16_t  binder     = -1;    // row index into the schema table, or -1
        uint16_t occurrence = 0;     // which occurrence of that row
        int32_t  unknownIdx = -1;    // index into `unknown` when binder == -1
        bool     viaXxxx    = false; // original framing used XXXX
    };
    std::vector<LayoutEntry> layout;
    std::vector<UnknownSub>  unknown;
};

// One schema-table row. Plain function pointers (captureless-lambda targets)
// keep the tables cheap to compile and link.
template <class Rec>
struct SubBinder {
    uint32_t sig = 0;
    DecodeResult (*decode)(Rec&, SubView, const SchemaCtx&) = nullptr;
    uint32_t (*count)(const Rec&) = nullptr;   // occurrences currently present
    void (*encode)(const Rec&, uint32_t occurrence, const SchemaCtx&, ByteWriter& payload) = nullptr;
    // FormID visitation (master derivation / remap); null when the row's type
    // carries no FormIDs.
    void (*visitIds)(Rec&, void (*fn)(uint32_t& id, void* ctx), void* ctx) = nullptr;
    // Per-record remap gate: returns false when this row's CURRENT data cannot
    // be safely FormID-remapped (e.g. a condition whose function index is
    // unknown, so its params' form-vs-int classification is unknown). null =
    // always safe. If any present row reports unsafe, remapping refuses rather
    // than risk corrupting an integer or missing a reference.
    bool (*remapSafe)(const Rec&) = nullptr;
};

template <class Rec, size_t N>
struct RecordSchema {
    using RecType = Rec;

    uint32_t                    sig = 0;
    std::array<SubBinder<Rec>, N> rows;
};

// ── Member-pointer traits ────────────────────────────────────────────────────

template <class MP> struct MemPtrTraits;
template <class C, class M> struct MemPtrTraits<M C::*> {
    using Class  = C;
    using Member = M;
};

// ── Codecs ───────────────────────────────────────────────────────────────────
// Codec contract:
//   static bool Decode(T& out, SubView, const SchemaCtx&);   // false = refuse
//   static void Encode(const T&, const SchemaCtx&, ByteWriter& payload);
//   (optional) static void VisitIds(T&, void (*fn)(uint32_t&, void*), void*);

struct ZString {
    static bool Decode(std::string& out, SubView s, const SchemaCtx&) {
        // Refuse a payload that is not exactly "chars + one NUL" — e.g. an
        // embedded NUL or a missing terminator could not be re-emitted.
        if (s.size == 0) return false;
        if (s.data[s.size - 1] != 0) return false;
        for (uint32_t i = 0; i + 1 < s.size; ++i)
            if (s.data[i] == 0) return false;
        out.assign(reinterpret_cast<const char*>(s.data), s.size - 1);
        return true;
    }
    static void Encode(const std::string& v, const SchemaCtx&, ByteWriter& w) {
        w.ZString(v);
    }
};

struct U32 {
    static bool Decode(uint32_t& out, SubView s, const SchemaCtx&) {
        if (s.size != 4) return false;
        std::memcpy(&out, s.data, 4);
        return true;
    }
    static void Encode(const uint32_t& v, const SchemaCtx&, ByteWriter& w) { w.U32(v); }
};

struct U16v {
    static bool Decode(uint16_t& out, SubView s, const SchemaCtx&) {
        if (s.size != 2) return false;
        std::memcpy(&out, s.data, 2);
        return true;
    }
    static void Encode(const uint16_t& v, const SchemaCtx&, ByteWriter& w) { w.U16(v); }
};

struct U8v {
    static bool Decode(uint8_t& out, SubView s, const SchemaCtx&) {
        if (s.size != 1) return false;
        out = s.data[0];
        return true;
    }
    static void Encode(const uint8_t& v, const SchemaCtx&, ByteWriter& w) { w.U8(v); }
};

struct F32 {
    static bool Decode(float& out, SubView s, const SchemaCtx&) {
        if (s.size != 4) return false;
        std::memcpy(&out, s.data, 4);
        return true;
    }
    static void Encode(const float& v, const SchemaCtx&, ByteWriter& w) { w.F32(v); }
};

// A record reference: raw 32-bit FormID (interpreted through the owning
// plugin's master list). The one type FormID visitation recognizes natively.
struct FormRef {
    uint32_t id = 0;
};

struct FormRefCodec {
    static bool Decode(FormRef& out, SubView s, const SchemaCtx&) {
        if (s.size != 4) return false;
        std::memcpy(&out.id, s.data, 4);
        return true;
    }
    static void Encode(const FormRef& v, const SchemaCtx&, ByteWriter& w) { w.U32(v.id); }
    static void VisitIds(FormRef& v, void (*fn)(uint32_t&, void*), void* ctx) {
        fn(v.id, ctx);
    }
};

// A trivially-copyable packed struct with an EXACT on-disk size. A payload of
// any other size is refused (→ passthrough), which is what keeps multi-sized
// vanilla subrecords byte-faithful until their variants are modeled.
// `IdOffsets`: byte offsets of the FormID fields inside T (empty = none).
struct FieldDef { uint32_t offset; };
constexpr FieldDef FormIdAt(uint32_t offset) { return FieldDef{ offset }; }

template <class T, auto& IdOffsets>
struct PackedIds {
    static_assert(std::is_trivially_copyable_v<T>);
    static bool Decode(T& out, SubView s, const SchemaCtx&) {
        if (s.size != sizeof(T)) return false;
        std::memcpy(&out, s.data, sizeof(T));
        return true;
    }
    static void Encode(const T& v, const SchemaCtx&, ByteWriter& w) {
        w.Bytes(&v, sizeof(T));
    }
    static void VisitIds(T& v, void (*fn)(uint32_t&, void*), void* ctx) {
        const std::span<const FieldDef> defs(IdOffsets);
        for (const FieldDef& f : defs) {
            uint32_t id = 0;
            std::memcpy(&id, reinterpret_cast<uint8_t*>(&v) + f.offset, 4);
            fn(id, ctx);
            std::memcpy(reinterpret_cast<uint8_t*>(&v) + f.offset, &id, 4);
        }
    }
};

template <class T>
struct Packed {
    static_assert(std::is_trivially_copyable_v<T>);
    static bool Decode(T& out, SubView s, const SchemaCtx&) {
        if (s.size != sizeof(T)) return false;
        std::memcpy(&out, s.data, sizeof(T));
        return true;
    }
    static void Encode(const T& v, const SchemaCtx&, ByteWriter& w) {
        w.Bytes(&v, sizeof(T));
    }
};

// Raw byte blob: accepts any payload verbatim. For subrecords whose content
// is opaque at this wave (MODT texture hashes, NAM8/… data) but whose
// PRESENCE the record model wants to expose.
struct Blob {
    static bool Decode(std::vector<uint8_t>& out, SubView s, const SchemaCtx&) {
        out.assign(s.data, s.data + s.size);
        return true;
    }
    static void Encode(const std::vector<uint8_t>& v, const SchemaCtx&, ByteWriter& w) {
        if (!v.empty()) w.Bytes(v.data(), v.size());
    }
};

// Which localized-strings sidecar a string id indexes into. Assignment is
// PER FIELD (fixed by the format): FULL-style short names → .STRINGS,
// book/description text → .DLSTRINGS, dialogue lines → .ILSTRINGS.
enum class StrVariant : uint8_t { Strings = 0, ILStrings = 1, DLStrings = 2 };

// A localized string: a u32 string-table id in localized plugins, an inline
// zstring otherwise. Encode reproduces whichever form was decoded (a plugin's
// localized flag is fixed, so the two never mix within one file). `variant`
// records which sidecar the id resolves through (set by the field's codec).
struct Lstring {
    uint32_t                   id = 0;      // valid when the plugin is localized
    std::optional<std::string> text;        // valid otherwise
    StrVariant                 variant = StrVariant::Strings;
};

template <StrVariant V>
struct LStringVar {
    static bool Decode(Lstring& out, SubView s, const SchemaCtx& ctx) {
        out.variant = V;
        if (ctx.localized) {
            if (s.size != 4) return false;
            std::memcpy(&out.id, s.data, 4);
            out.text.reset();
            return true;
        }
        std::string t;
        if (!ZString::Decode(t, s, ctx)) return false;
        out.text = std::move(t);
        return true;
    }
    static void Encode(const Lstring& v, const SchemaCtx& ctx, ByteWriter& w) {
        if (ctx.localized) w.U32(v.id);
        else               w.ZString(v.text ? *v.text : std::string());
    }
};

using LString   = LStringVar<StrVariant::Strings>;
using LStringDL = LStringVar<StrVariant::DLStrings>;
using LStringIL = LStringVar<StrVariant::ILStrings>;

// A marker subrecord: zero-size, presence-only (MNAM/FNAM gender markers,
// NAM0/NAM1 section markers in RACE, …). Stored as a count because markers
// legitimately repeat.
struct Marker {
    static bool Decode(std::monostate&, SubView s, const SchemaCtx&) {
        return s.size == 0;
    }
    static void Encode(const std::monostate&, const SchemaCtx&, ByteWriter&) {}
};

// ── Binder factories ─────────────────────────────────────────────────────────

namespace schema_detail {

template <class Codec, class T>
constexpr auto VisitThunkFor() {
    if constexpr (requires(T& v) { Codec::VisitIds(v, nullptr, nullptr); })
        return true;
    else
        return false;
}

} // namespace schema_detail

// Singular field: member is std::optional<T>. A second on-disk occurrence is
// refused (→ passthrough) so it cannot be silently collapsed.
template <auto MemberPtr, class Codec>
constexpr auto Field(const char sig[5]) {
    using Rec = typename MemPtrTraits<decltype(MemberPtr)>::Class;
    using Opt = typename MemPtrTraits<decltype(MemberPtr)>::Member;
    using T   = typename Opt::value_type;

    SubBinder<Rec> b{};
    b.sig = Sig4(sig);
    b.decode = [](Rec& r, SubView s, const SchemaCtx& ctx) -> DecodeResult {
        if ((r.*MemberPtr).has_value()) return DecodeResult::Passthrough;
        T v{};
        if (!Codec::Decode(v, s, ctx)) return DecodeResult::Passthrough;
        (r.*MemberPtr) = std::move(v);
        return DecodeResult::Consumed;
    };
    b.count = [](const Rec& r) -> uint32_t {
        return (r.*MemberPtr).has_value() ? 1u : 0u;
    };
    b.encode = [](const Rec& r, uint32_t, const SchemaCtx& ctx, ByteWriter& w) {
        Codec::Encode(*(r.*MemberPtr), ctx, w);
    };
    if constexpr (schema_detail::VisitThunkFor<Codec, T>()) {
        b.visitIds = [](Rec& r, void (*fn)(uint32_t&, void*), void* ctx) {
            if ((r.*MemberPtr).has_value())
                Codec::VisitIds(*(r.*MemberPtr), fn, ctx);
        };
    }
    return b;
}

// Repeating field: member is std::vector<T>; each on-disk occurrence appends.
template <auto MemberPtr, class Codec>
constexpr auto Repeated(const char sig[5]) {
    using Rec = typename MemPtrTraits<decltype(MemberPtr)>::Class;
    using Vec = typename MemPtrTraits<decltype(MemberPtr)>::Member;
    using T   = typename Vec::value_type;

    SubBinder<Rec> b{};
    b.sig = Sig4(sig);
    b.decode = [](Rec& r, SubView s, const SchemaCtx& ctx) -> DecodeResult {
        T v{};
        if (!Codec::Decode(v, s, ctx)) return DecodeResult::Passthrough;
        (r.*MemberPtr).push_back(std::move(v));
        return DecodeResult::Consumed;
    };
    b.count = [](const Rec& r) -> uint32_t {
        return static_cast<uint32_t>((r.*MemberPtr).size());
    };
    b.encode = [](const Rec& r, uint32_t occ, const SchemaCtx& ctx, ByteWriter& w) {
        Codec::Encode((r.*MemberPtr)[occ], ctx, w);
    };
    if constexpr (schema_detail::VisitThunkFor<Codec, T>()) {
        b.visitIds = [](Rec& r, void (*fn)(uint32_t&, void*), void* ctx) {
            for (T& v : (r.*MemberPtr)) Codec::VisitIds(v, fn, ctx);
        };
    }
    return b;
}

template <class Rec, class... Rows>
constexpr auto MakeSchema(const char sig[5], Rows... rows) {
    return RecordSchema<Rec, sizeof...(Rows)>{ Sig4(sig), { rows... } };
}

// ── Drivers ──────────────────────────────────────────────────────────────────

template <class Rec, size_t N>
bool DecodeRecord(const RecordSchema<Rec, N>& schema,
                  std::span<const uint8_t> payload,
                  const SchemaCtx& ctx, Rec& out) {
    out = Rec{};
    std::array<uint16_t, N> occ{};

    SubIterator it(payload);
    SubView sub;
    while (it.Next(sub)) {
        int row = -1;
        for (size_t i = 0; i < N; ++i)
            if (schema.rows[i].sig == sub.sig) { row = static_cast<int>(i); break; }

        RecordExtras::LayoutEntry e;
        e.viaXxxx = sub.viaXxxx;
        if (row >= 0 &&
            schema.rows[static_cast<size_t>(row)].decode(out, sub, ctx) ==
                DecodeResult::Consumed) {
            e.binder     = static_cast<int16_t>(row);
            e.occurrence = occ[static_cast<size_t>(row)]++;
        } else {
            e.unknownIdx = static_cast<int32_t>(out.unknown.size());
            out.unknown.push_back(
                UnknownSub{ sub.sig, sub.viaXxxx,
                            std::vector<uint8_t>(sub.data, sub.data + sub.size) });
        }
        out.layout.push_back(e);
    }
    return it.ok();
}

template <class Rec, size_t N>
void EncodeRecord(const RecordSchema<Rec, N>& schema, const Rec& rec,
                  const SchemaCtx& ctx, ByteWriter& out) {
    std::array<uint32_t, N> emitted{};
    ByteWriter payload;

    auto emitRow = [&](size_t row, uint32_t occurrence, bool viaXxxx) {
        payload.buf.clear();
        schema.rows[row].encode(rec, occurrence, ctx, payload);
        EmitSubrecord(out, schema.rows[row].sig,
                      payload.buf.data(), payload.buf.size(), viaXxxx);
    };

    // 1. Replay the original sequence.
    for (const RecordExtras::LayoutEntry& e : rec.layout) {
        if (e.binder >= 0) {
            const size_t row = static_cast<size_t>(e.binder);
            if (e.occurrence < schema.rows[row].count(rec)) {
                emitRow(row, e.occurrence, e.viaXxxx);
                if (e.occurrence >= emitted[row]) emitted[row] = e.occurrence + 1u;
            }
            // An occurrence the mutated record no longer has simply drops out.
        } else if (e.unknownIdx >= 0) {
            const UnknownSub& u = rec.unknown[static_cast<size_t>(e.unknownIdx)];
            EmitSubrecord(out, u.sig, u.bytes.data(), u.bytes.size(), u.viaXxxx);
        }
    }

    // 2. Append occurrences that never had a layout slot (fields set after
    //    decode, or a brand-new record), in schema-table order.
    for (size_t row = 0; row < N; ++row) {
        const uint32_t present = schema.rows[row].count(rec);
        for (uint32_t o = emitted[row]; o < present; ++o)
            emitRow(row, o, false);
    }
}

template <class Rec, size_t N>
void VisitFormIds(const RecordSchema<Rec, N>& schema, Rec& rec,
                  void (*fn)(uint32_t& id, void* ctx), void* ctx) {
    for (size_t i = 0; i < N; ++i)
        if (schema.rows[i].visitIds) schema.rows[i].visitIds(rec, fn, ctx);
}

// True when every subrecord the record carries was semantically consumed —
// i.e. nothing sits in unknown-passthrough. Useful for coverage metrics.
inline bool FullyModeled(const RecordExtras& rec) { return rec.unknown.empty(); }

} // namespace esper
