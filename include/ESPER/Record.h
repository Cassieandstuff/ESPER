#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// esper::Record — the schema-driven record model. One generic engine
// interprets a SchemaSet (SchemaTypes.h) for every record type; nothing here
// is per-record code. See docs/schema-language.md for the semantics.
//
//   for (esper::RecordNode* node : plugin.Records(esper::Sig4("ARMO"))) {
//       auto rec = esper::Record::Decode(plugin, *node);      // Result<Record>
//       if (!rec) { log(rec.error().message); continue; }
//       auto armor = esper::view::As<esper::view::Armor>(*rec); // nullopt if not ARMO
//       armor->SetEditorId("X");                              // marks that subrecord dirty
//       esper::Record::Commit(plugin, *node, *rec);
//   }
//
// Fidelity: every subrecord keeps its original bytes. Encode emits them
// verbatim unless the subrecord was modified (or `forceReencode`, which the
// real-data gate uses to prove every layout re-encodes byte-exactly). A
// subrecord the schema can't reproduce exactly is kept raw.
// ─────────────────────────────────────────────────────────────────────────────

#include <ESPER/ByteWriter.h>
#include <ESPER/Error.h>
#include <ESPER/SchemaTypes.h>

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace esper {

struct Plugin;
struct RecordNode;

// An on-disk FormID: the top byte indexes the owning plugin's [masters..., self].
struct FormId {
    uint32_t raw = 0;
    friend bool operator==(FormId, FormId) = default;
};

// A typed reference: a FormId whose schema declares exactly one target record
// type that has a generated view. Resolving it needs a load order.
template <class View>
struct Link {
    FormId id;
    friend bool operator==(const Link&, const Link&) = default;
};

// An lstring: `id` is meaningful when the plugin is localized, `text` otherwise.
// `table` is the string table the schema assigns the field (FULL: strings,
// DESC: dlstrings, ...), so resolving it needs only the owning plugin.
struct LocalizedString {
    uint32_t          id = 0;
    std::string       text;
    schema::StrTable  table = schema::StrTable::Strings;
    friend bool operator==(const LocalizedString&, const LocalizedString&) = default;
};

// A decoded value. Scalars, FormIDs, f32 bits and lstring ids live in `bits`
// (floats are stored as their raw bits so NaN payloads survive); strings in
// `str`; opaque bytes in `bytes`; struct fields and array elements in `items`.
// A struct decoded from a truncated (break) form has `present` < its field count.
struct Value {
    static constexpr uint16_t kAll = 0xFFFF;
    uint64_t             bits    = 0;
    std::string          str;
    std::vector<uint8_t> bytes;
    std::vector<Value>   items;
    uint16_t             present = kAll;
    friend bool operator==(const Value&, const Value&) = default;
};

// A path from a scope (a record root or a group instance): `memberDepth`
// member indices descend through nested groups to a subrecord member, then the
// remaining indices descend through struct fields (breaks not counted).
struct RelPath {
    std::array<uint16_t, 8> idx{};
    uint8_t memberDepth = 0;
    uint8_t depth       = 0;
};

// `p` extended by one step (a struct field index or a union case index).
inline RelPath Extend(RelPath p, uint16_t i) {
    p.idx[p.depth++] = i;
    return p;
}

// Generated handles (include/ESPER/generated/Fields.h). `T` is the value type
// the schema gives the field, so a wrong name or type fails to compile.
template <class T>
struct FieldRef {
    uint32_t record = 0;
    RelPath  path;
};
struct GroupRef {
    uint32_t record = 0;
    RelPath  path;
};
struct StructRef {
    uint32_t record = 0;
    RelPath  path;
};

struct DecodeCtx {
    bool     localized   = false;   // the owning plugin's TES4 localized flag
    uint16_t formVersion = 0;
};

class Record;

// A position in a record: the record root (inst 0) or one group instance.
// Generated views are thin wrappers around a Cursor.
struct Cursor {
    Record*  rec  = nullptr;
    uint32_t inst = 0;

    template <class T> std::optional<T> Get(const RelPath& p) const;
    template <class T> std::vector<T>   GetAll(const RelPath& p) const;
    template <class T> void             Set(const RelPath& p, const T& v) const;
    template <class T> void             SetAll(const RelPath& p, const std::vector<T>& v) const;
    bool                                Has(const RelPath& p) const;
    void                                Clear(const RelPath& p) const;
    std::vector<Cursor>                 Groups(const RelPath& p) const;
    Cursor                              EnsureGroup(const RelPath& p) const;
    Cursor                              AddGroup(const RelPath& p) const;
};

// A struct value inside a subrecord, addressed by a path prefix from a Cursor.
struct StructCursor {
    Cursor  at;
    RelPath prefix;

    RelPath Field(uint16_t index) const {
        RelPath p = prefix;
        p.idx[p.depth++] = index;
        return p;
    }
    StructCursor Nested(uint16_t index) const { return { at, Field(index) }; }
    bool Has() const { return at.Has(prefix); }
};

class Record {
public:
    // A parsed plugin's record, decoded with schema::Main(). Fails when the
    // payload won't inflate, there's no schema for its type, or the
    // subrecord stream is malformed.
    static Result<Record> Decode(const Plugin& plugin, RecordNode& node);
    // A new, empty record of `sig` for `plugin`.
    static Result<Record> Create(const Plugin& plugin, uint32_t sig);
    // Writes the encoded record back into `node`, which must be the same
    // record type (SetRecordPayload).
    static Result<void> Commit(Plugin& plugin, RecordNode& node, const Record& rec);

    // The same against an explicit schema set and context (tests, tools).
    static Result<Record> Decode(const schema::SchemaSet& set, uint32_t sig,
                                 std::span<const uint8_t> payload, const DecodeCtx& ctx);
    static Result<Record> Create(const schema::SchemaSet& set, uint32_t sig, const DecodeCtx& ctx);

    // Encodes the payload. Unmodified subrecords are emitted verbatim unless
    // `forceReencode`, which re-encodes every modeled subrecord from its value.
    void                 Encode(ByteWriter& w, bool forceReencode = false) const;
    std::vector<uint8_t> Encode(bool forceReencode = false) const;

    uint32_t                    Sig() const { return def_ ? def_->sig : 0; }
    const schema::RecordDef&    Def() const { return *def_; }
    const schema::SchemaSet&    Schema() const { return *set_; }
    const DecodeCtx&            Context() const { return ctx_; }
    Cursor                      Root() { return { this, 0 }; }

    // Subrecords the schema could not model (kept verbatim).
    size_t                RawCount() const;
    std::vector<uint32_t> RawSigs() const;
    size_t                SubrecordCount() const { return nodes_.size(); }

    // ── Typed access through generated handles ──────────────────────────────
    template <class T> std::optional<T> Get(const FieldRef<T>& f) const { return GetAt<T>(0, f.path); }
    template <class T> std::vector<T>   GetAll(const FieldRef<T>& f) const { return GetAllAt<T>(0, f.path); }
    template <class T> void Set(const FieldRef<T>& f, const T& v) { SetAt<T>(0, f.path, v); }
    template <class T> void SetAll(const FieldRef<T>& f, const std::vector<T>& v) { SetAllAt<T>(0, f.path, v); }
    template <class T> bool Has(const FieldRef<T>& f) const { return HasAt(0, f.path); }
    template <class T> void Clear(const FieldRef<T>& f) { ClearAt(0, f.path); }
    std::vector<Cursor> Groups(const GroupRef& g) { return GroupsAt(0, g.path); }

    // ── Untyped access (tools/UIs): "editorId", "maleModel/path", "data/weight"
    std::optional<RelPath> Find(std::string_view path) const;
    const Value*           GetValue(const RelPath& p) const { return FindValue(0, p); }

    // ── FormIDs ─────────────────────────────────────────────────────────────
    // Every FormID the record holds is visible to the schema: no raw
    // subrecords, and no non-empty `bytes` value that isn't declared free of
    // FormIDs. The error names the first blocker.
    Result<void> CheckRemapSafe() const;
    bool         RemapSafe() const { return CheckRemapSafe().has_value(); }
    // Applies `fn` to every FormID (marking changed subrecords dirty). Refuses,
    // touching nothing, when the record isn't remap-safe.
    Result<void> VisitFormIds(const std::function<void(FormId& id)>& fn);
    // Read-only enumeration of every visible FormID with its declared targets
    // (empty span = any record type), in subrecord order.
    void ForEachFormId(const std::function<void(FormId id, std::span<const uint32_t> to)>& fn) const;

    // ── Localization ────────────────────────────────────────────────────────
    // Re-targets the record to a plugin whose localized flag is `localized`.
    // Localized → inline: each lstring id is replaced by `lookup(id, table)`.
    // Inline → localized is refused when the record holds any lstring (new
    // string-table entries can't be minted here). Refuses, touching nothing,
    // when a raw subrecord could hide string ids or a lookup misses.
    Result<void> Relocalize(bool localized,
                            const std::function<std::optional<std::string>(uint32_t id, schema::StrTable)>& lookup);

    // ── Cursor-level primitives (used by Cursor and the generated views) ─────
    template <class T> std::optional<T> GetAt(uint32_t inst, const RelPath& p) const;
    template <class T> std::vector<T>   GetAllAt(uint32_t inst, const RelPath& p) const;
    template <class T> void             SetAt(uint32_t inst, const RelPath& p, const T& v);
    template <class T> void             SetAllAt(uint32_t inst, const RelPath& p, const std::vector<T>& v);
    bool                HasAt(uint32_t inst, const RelPath& p) const { return FindValue(inst, p) != nullptr; }
    void                ClearAt(uint32_t inst, const RelPath& p);
    std::vector<Cursor> GroupsAt(uint32_t inst, const RelPath& p);
    Cursor              EnsureGroupAt(uint32_t inst, const RelPath& p);
    Cursor              AddGroupAt(uint32_t inst, const RelPath& p);

    const Value*                     FindValue(uint32_t inst, const RelPath& p) const;
    // The value of the latest subrecord member named `name` in instance `inst`,
    // or else in its enclosing instances (nearest first); nullptr if none.
    const Value*                     LatestMemberValue(uint32_t inst, std::string_view name) const;
    std::vector<const Value*>        FindAllValues(uint32_t inst, const RelPath& p) const;
    const schema::TypeDef*           TypeAt(uint32_t inst, const RelPath& p) const;
    // Replaces the value at `p` (creating groups/subrecord as needed). A no-op
    // when the value is unchanged, so untouched bytes stay verbatim.
    void                             StoreValue(uint32_t inst, const RelPath& p, Value v);
    // Replaces every occurrence of a repeated subrecord member.
    void                             StoreAll(uint32_t inst, const RelPath& p, std::vector<Value> vs);

    // T <-> Value given the TypeDef the value lives in.
    template <class T> static T     FromValue(const Value& v, const schema::TypeDef& t, const schema::SchemaSet& s);
    template <class T> static Value ToValue(const T& x, const schema::TypeDef& t, const schema::SchemaSet& s);

private:
    struct Inst {
        uint16_t group  = 0;
        int32_t  parent = -1;
        uint16_t slot   = 0;    // member index within the parent's group
    };
    struct Node {
        uint32_t      sig     = 0;
        uint32_t      off     = 0;    // into bytes_ (original payload)
        uint32_t      size    = 0;
        uint32_t      inst    = 0;
        int16_t       member  = -1;   // -1 = raw
        bool          viaXxxx = false;
        bool          dirty   = false;
        mutable bool  decoded = false;
        mutable Value value;
    };

    const schema::GroupDef&  GroupOf(uint32_t inst) const;
    const schema::MemberDef& MemberOf(uint32_t inst, uint16_t m) const;
    const Value&             NodeValue(const Node& n) const;
    const Value*             Descend(const Value& root, uint16_t type, const RelPath& p) const;
    int32_t                  ChildAtLevel(uint32_t nodeInst, uint32_t level) const;
    std::vector<uint32_t>    InstancesOf(uint32_t parent, uint16_t slot) const;
    int32_t                  FirstNode(uint32_t inst, uint16_t member) const;
    size_t                   InsertPos(uint32_t inst, uint16_t slot) const;
    // Walks the member part of `p` from `inst`. Returns the scope holding the
    // final subrecord member, or -1 when a group on the way is absent.
    int32_t                  WalkGroups(uint32_t inst, const RelPath& p, bool create);
    int32_t                  WalkGroupsConst(uint32_t inst, const RelPath& p) const;
    const schema::TypeDef&   FieldType(const schema::TypeDef& structType, uint16_t valueIndex) const;
    void                     UpdateCount(uint32_t inst, uint16_t member);
    Value                    DefaultValue(uint16_t type) const;

    const schema::SchemaSet* set_ = nullptr;
    const schema::RecordDef* def_ = nullptr;
    DecodeCtx                ctx_;
    std::vector<uint8_t>     bytes_;
    std::vector<Node>        nodes_;
    std::vector<Inst>        insts_;

    friend struct RecordAccess;
};

// ── Value conversions ────────────────────────────────────────────────────────

namespace record_detail {
template <class T> struct IsVector : std::false_type {};
template <class T> struct IsVector<std::vector<T>> : std::true_type { using Elem = T; };
template <class T> struct IsLink : std::false_type {};
template <class V> struct IsLink<Link<V>> : std::true_type {};

inline uint64_t MaskFor(schema::Kind k) {
    switch (k) {
    case schema::Kind::U8:  case schema::Kind::I8:  return 0xFFull;
    case schema::Kind::U16: case schema::Kind::I16: return 0xFFFFull;
    case schema::Kind::U64: case schema::Kind::I64: return ~0ull;
    default:                                        return 0xFFFFFFFFull;
    }
}
} // namespace record_detail

template <class T>
T Record::FromValue(const Value& v, const schema::TypeDef& t, const schema::SchemaSet& s) {
    if constexpr (std::is_same_v<T, Value>) {
        return v;
    } else if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<float>(static_cast<uint32_t>(v.bits));
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<T>(v.bits);
    } else if constexpr (std::is_same_v<T, FormId>) {
        return FormId{ static_cast<uint32_t>(v.bits) };
    } else if constexpr (record_detail::IsLink<T>::value) {
        return T{ FormId{ static_cast<uint32_t>(v.bits) } };
    } else if constexpr (std::is_same_v<T, std::string>) {
        return v.str;
    } else if constexpr (std::is_same_v<T, LocalizedString>) {
        return LocalizedString{ static_cast<uint32_t>(v.bits), v.str, t.table };
    } else if constexpr (record_detail::IsVector<T>::value) {
        using E = typename record_detail::IsVector<T>::Elem;
        T out;
        if (t.kind == schema::Kind::Bytes) {
            if constexpr (std::is_same_v<E, uint8_t>) out = v.bytes;
            return out;
        }
        const schema::TypeDef& et = s.types[t.ref];
        out.reserve(v.items.size());
        for (const Value& item : v.items) out.push_back(FromValue<E>(item, et, s));
        return out;
    } else {
        static_assert(sizeof(T) == 0, "esper::Record: unsupported value type");
    }
}

template <class T>
Value Record::ToValue(const T& x, const schema::TypeDef& t, const schema::SchemaSet& s) {
    Value v;
    if constexpr (std::is_same_v<T, Value>) {
        v = x;
    } else if constexpr (std::is_same_v<T, float>) {
        v.bits = std::bit_cast<uint32_t>(x);
    } else if constexpr (std::is_integral_v<T>) {
        v.bits = static_cast<uint64_t>(x) & record_detail::MaskFor(t.kind);
    } else if constexpr (std::is_same_v<T, FormId>) {
        v.bits = x.raw;
    } else if constexpr (record_detail::IsLink<T>::value) {
        v.bits = x.id.raw;
    } else if constexpr (std::is_same_v<T, std::string>) {
        v.str = x;
    } else if constexpr (std::is_same_v<T, LocalizedString>) {
        v.bits = x.id;
        v.str  = x.text;
    } else if constexpr (record_detail::IsVector<T>::value) {
        using E = typename record_detail::IsVector<T>::Elem;
        if (t.kind == schema::Kind::Bytes) {
            if constexpr (std::is_same_v<E, uint8_t>) v.bytes = x;
            return v;
        }
        const schema::TypeDef& et = s.types[t.ref];
        v.items.reserve(x.size());
        for (const E& e : x) v.items.push_back(ToValue<E>(e, et, s));
    } else {
        static_assert(sizeof(T) == 0, "esper::Record: unsupported value type");
    }
    return v;
}

template <class T>
std::optional<T> Record::GetAt(uint32_t inst, const RelPath& p) const {
    const Value* v = FindValue(inst, p);
    if (!v) return std::nullopt;
    return FromValue<T>(*v, *TypeAt(inst, p), *set_);
}

template <class T>
std::vector<T> Record::GetAllAt(uint32_t inst, const RelPath& p) const {
    std::vector<T> out;
    const schema::TypeDef* t = TypeAt(inst, p);
    if (!t) return out;
    for (const Value* v : FindAllValues(inst, p)) out.push_back(FromValue<T>(*v, *t, *set_));
    return out;
}

template <class T>
void Record::SetAt(uint32_t inst, const RelPath& p, const T& x) {
    const schema::TypeDef* t = TypeAt(inst, p);
    if (!t) return;
    StoreValue(inst, p, ToValue<T>(x, *t, *set_));
}

template <class T>
void Record::SetAllAt(uint32_t inst, const RelPath& p, const std::vector<T>& xs) {
    const schema::TypeDef* t = TypeAt(inst, p);
    if (!t) return;
    std::vector<Value> vs;
    vs.reserve(xs.size());
    for (const T& x : xs) vs.push_back(ToValue<T>(x, *t, *set_));
    StoreAll(inst, p, std::move(vs));
}

template <class T> std::optional<T> Cursor::Get(const RelPath& p) const { return rec->GetAt<T>(inst, p); }
template <class T> std::vector<T>   Cursor::GetAll(const RelPath& p) const { return rec->GetAllAt<T>(inst, p); }
template <class T> void Cursor::Set(const RelPath& p, const T& v) const { rec->SetAt<T>(inst, p, v); }
template <class T> void Cursor::SetAll(const RelPath& p, const std::vector<T>& v) const { rec->SetAllAt<T>(inst, p, v); }
inline bool                Cursor::Has(const RelPath& p) const { return rec->HasAt(inst, p); }
inline void                Cursor::Clear(const RelPath& p) const { rec->ClearAt(inst, p); }
inline std::vector<Cursor> Cursor::Groups(const RelPath& p) const { return rec->GroupsAt(inst, p); }
inline Cursor              Cursor::EnsureGroup(const RelPath& p) const { return rec->EnsureGroupAt(inst, p); }
inline Cursor              Cursor::AddGroup(const RelPath& p) const { return rec->AddGroupAt(inst, p); }

} // namespace esper
