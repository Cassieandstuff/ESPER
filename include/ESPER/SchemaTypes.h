#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// The compiled form of an ESPER schema: flat constexpr tables, linked by index,
// that the generic engine (Record.h) interprets. tools/schemagen emits one
// SchemaSet per descriptor tree (include/ESPER/generated/Tables.h for the real
// schema); nothing here is hand-maintained per record.
//
// Table layout (all indices are into the same SchemaSet):
//   TypeDef    a value layout: scalar, string, bytes, struct (→ FieldDef range)
//              or array (→ element TypeDef). FormId types carry their allowed
//              link targets (→ links range).
//   FieldDef   one struct field, or a `break` (payload may end here).
//   MemberDef  one member of a group scope: a subrecord (sig + TypeDef) or a
//              nested group (→ GroupDef).
//   EntryDef   per group: which incoming sig selects which member. A nested
//              group is entered by any of its own entry sigs, so the generator
//              flattens those here and guarantees each sig is unique per scope.
//   GroupDef   a scope: a member range + entry range. A record's root scope is
//              unordered; nested groups follow the ordinal rule.
//   RecordDef  a record signature → its root GroupDef.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <span>

namespace esper::schema {

enum class Kind : uint8_t {
    U8, U16, U32, U64, I8, I16, I32, I64, F32,
    FormId,     // u32; the only kind FormID visitation touches
    Sig4,       // 4-char tag stored as u32
    ZString,    // NUL-terminated
    WString,    // u16 length + bytes
    LString,    // u32 string id when the plugin is localized, else ZString
    Bytes,      // opaque
    Struct,
    Array,
};

enum class LenKind : uint8_t {
    None,
    Fixed,      // Bytes: `count` bytes; Array: `count` elements
    Rest,       // to the end of the subrecord
    Prefix8,    // Array: element count read from a leading u8 / u16 / u32
    Prefix16,
    Prefix32,
};

enum class StrTable : uint8_t { None, Strings, DLStrings, ILStrings };

inline constexpr uint8_t kFormIdsNone = 0x01;   // Bytes: declared free of FormIDs

struct TypeDef {
    Kind        kind      = Kind::U8;
    LenKind     len       = LenKind::None;
    StrTable    table     = StrTable::None;
    uint8_t     flags     = 0;
    uint32_t    count     = 0;   // Fixed Bytes: byte count; Fixed Array: element count
    uint16_t    ref       = 0;   // Array: element TypeDef; Struct: first FieldDef
    uint16_t    refCount  = 0;   // Struct: FieldDef count (breaks included)
    uint16_t    linkFirst = 0;   // FormId: allowed target sigs (empty = any)
    uint16_t    linkCount = 0;
    const char* name      = nullptr;  // named struct type, for diagnostics
};

struct FieldDef {
    const char* name    = nullptr;
    uint16_t    type    = 0;
    bool        isBreak = false;
};

struct MemberDef {
    const char* name     = nullptr;
    uint32_t    sig      = 0;       // subrecord members
    uint16_t    type     = 0;       // subrecord members: the payload's TypeDef
    uint16_t    group    = 0;       // group members: the nested GroupDef
    bool        isGroup  = false;
    bool        repeat   = false;
    int16_t     countOf  = -1;      // derived count of this member index (same scope)
};

struct EntryDef {
    uint32_t sig    = 0;
    uint16_t member = 0;            // index within the group's member range
};

struct GroupDef {
    const char* name        = nullptr;
    uint16_t    firstMember = 0;
    uint16_t    memberCount = 0;
    uint16_t    firstEntry  = 0;
    uint16_t    entryCount  = 0;
    bool        ordered     = true;  // false for a record's root scope
};

struct RecordDef {
    uint32_t    sig  = 0;
    const char* name = nullptr;
    uint16_t    root = 0;            // GroupDef
};

struct SchemaSet {
    std::span<const TypeDef>   types;
    std::span<const FieldDef>  fields;
    std::span<const uint32_t>  links;
    std::span<const MemberDef> members;
    std::span<const EntryDef>  entries;
    std::span<const GroupDef>  groups;
    std::span<const RecordDef> records;     // sorted by sig
    const char*                schemaVersion   = nullptr;
    const char*                languageVersion = nullptr;

    const RecordDef* Find(uint32_t sig) const {
        size_t lo = 0, hi = records.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (records[mid].sig < sig) lo = mid + 1;
            else hi = mid;
        }
        return (lo < records.size() && records[lo].sig == sig) ? &records[lo] : nullptr;
    }
};

// constexpr signature helper for generated code: SigOf("ARMO").
constexpr uint32_t SigOf(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
}

// The schema generated from schema/ (include/ESPER/generated/Tables.h).
const SchemaSet& Main();

} // namespace esper::schema
