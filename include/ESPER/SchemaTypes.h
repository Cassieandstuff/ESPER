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
//              unordered; nested groups follow the ordinal rule, except
//              delimited groups (open/end markers), which are unordered inside.
//   RecordDef  a record signature → its root GroupDef.
//   CaseDef    one case of a union: the values it matches (→ caseValues, or a
//              min/max range) and its payload type; a union's default case is last.
//   LookupDef  a (table, column) a union selects on: key → value rows, sorted by key.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

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
    Union,      // a layout chosen by a selector (→ CaseDef range)
};

// What a union's case is chosen by.
enum class Selector : uint8_t {
    None,
    Size,         // bytes remaining in the subrecord at the union's position
    FormVersion,  // the record header's form version
    Record,       // the enclosing record's signature
    Field,        // an earlier field's value (optionally masked), by name
    Lookup,       // a table column, keyed by an earlier field's value
    Member,       // an earlier subrecord member's value, by name (same or enclosing scope)
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

inline constexpr uint8_t kFormIdsNone   = 0x01;   // Bytes: declared free of FormIDs
inline constexpr uint8_t kRemapUnsafe   = 0x02;   // any present value blocks FormID remap

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
    // Union only (ref/refCount = the CaseDef range):
    Selector    selector  = Selector::None;
    const char* selField  = nullptr;  // Field / Lookup: the key field's name; Member: the member's
    uint64_t    selMask   = 0;        // Field: AND mask (0 = none)
    uint16_t    lookup    = 0;        // Lookup: LookupDef
};

struct CaseDef {
    const char* name       = nullptr;  // optional; names a typed accessor
    uint16_t    type       = 0;        // payload TypeDef (unused when empty)
    bool        empty      = false;    // matches, but carries no bytes
    bool        isDefault  = false;
    bool        isRange    = false;
    int64_t     min        = 0;        // isRange: inclusive bounds
    int64_t     max        = 0;
    uint16_t    firstValue = 0;        // !isRange: matched values (→ caseValues)
    uint16_t    valueCount = 0;
};

struct LookupRow {
    uint32_t key   = 0;
    uint32_t value = 0;               // an interned value id, matched by cases
};

struct LookupDef {
    const char* name     = nullptr;   // "table.column"
    uint16_t    firstRow = 0;
    uint16_t    rowCount = 0;
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
    bool        ordered     = true;  // false for a record's root scope and delimited groups
    uint32_t    openSig     = 0;     // delimited: only this sig (the first member) opens an instance
    uint32_t    endSig      = 0;     // delimited: placing this sig (the last member) closes it
};

struct RecordDef {
    uint32_t    sig  = 0;
    const char* name = nullptr;
    uint16_t    root = 0;            // GroupDef
};

// A schema table (schema/tables/*.yaml) for consumers: every column as text,
// rows sorted by their integer key. Read it with SchemaSet::TableValue.
struct TableDef {
    const char* name        = nullptr;
    uint16_t    firstColumn = 0;    // into SchemaSet::tableColumns
    uint16_t    columnCount = 0;
    uint32_t    firstRow    = 0;    // into SchemaSet::tableKeys
    uint32_t    rowCount    = 0;
    uint32_t    firstCell   = 0;    // into SchemaSet::tableCells, row-major
};

struct SchemaSet {
    std::span<const TypeDef>   types;
    std::span<const FieldDef>  fields;
    std::span<const uint32_t>  links;
    std::span<const MemberDef> members;
    std::span<const EntryDef>  entries;
    std::span<const GroupDef>  groups;
    std::span<const RecordDef> records;     // sorted by sig
    std::span<const CaseDef>   cases;
    std::span<const uint64_t>  caseValues;
    std::span<const LookupDef> lookups;
    std::span<const LookupRow> lookupRows;
    std::span<const TableDef>  tables;
    std::span<const char* const> tableColumns;
    std::span<const uint32_t>  tableKeys;
    std::span<const char* const> tableCells;
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

    // The `column` cell of the row keyed `key` in `table`, as written in the
    // schema: TableValue("ctdaFunctions", 58, "name") == "GetStage". nullptr
    // when the table, row or column doesn't exist.
    const char* TableValue(std::string_view table, uint32_t key, std::string_view column) const {
        for (const TableDef& t : tables) {
            if (table != t.name) continue;
            int col = -1;
            for (uint16_t c = 0; c < t.columnCount; ++c)
                if (column == tableColumns[t.firstColumn + c]) col = c;
            if (col < 0) return nullptr;
            size_t lo = 0, hi = t.rowCount;
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                if (tableKeys[t.firstRow + mid] < key) lo = mid + 1;
                else hi = mid;
            }
            if (lo == t.rowCount || tableKeys[t.firstRow + lo] != key) return nullptr;
            return tableCells[t.firstCell + lo * t.columnCount + static_cast<size_t>(col)];
        }
        return nullptr;
    }
};

// The element width of an array the engine stores packed: an array whose
// element is a plain number (not a FormID, string or composite) keeps its
// elements as raw little-endian bytes in Value::bytes instead of one Value per
// element. 0 when `arrayType` isn't such an array.
constexpr size_t PackedElementWidth(const SchemaSet& s, const TypeDef& arrayType) {
    if (arrayType.kind != Kind::Array) return 0;
    switch (s.types[arrayType.ref].kind) {
    case Kind::U8:  case Kind::I8:  return 1;
    case Kind::U16: case Kind::I16: return 2;
    case Kind::U32: case Kind::I32: case Kind::F32: case Kind::Sig4: return 4;
    case Kind::U64: case Kind::I64: return 8;
    default: return 0;
    }
}

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
