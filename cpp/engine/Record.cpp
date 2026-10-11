#include <ESPER/Record.h>

#include <ESPER/Plugin.h>
#include <ESPER/Subrecords.h>
#include <ESPER/Writer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace esper {

using schema::Kind;
using schema::LenKind;
using schema::SchemaSet;
using schema::TypeDef;

namespace {

size_t ScalarWidth(Kind k) {
    switch (k) {
    case Kind::U8:  case Kind::I8:  return 1;
    case Kind::U16: case Kind::I16: return 2;
    case Kind::U64: case Kind::I64: return 8;
    case Kind::U32: case Kind::I32: case Kind::F32:
    case Kind::FormId: case Kind::Sig4: return 4;
    default: return 0;
    }
}

bool ReadLE(const uint8_t*& p, const uint8_t* end, size_t n, uint64_t& out) {
    if (static_cast<size_t>(end - p) < n) return false;
    out = 0;
    for (size_t i = 0; i < n; ++i) out |= static_cast<uint64_t>(p[i]) << (8 * i);
    p += n;
    return true;
}

void WriteLE(ByteWriter& w, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) w.U8(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

bool ReadZ(const uint8_t*& p, const uint8_t* end, Value* out) {
    const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(p, 0, static_cast<size_t>(end - p)));
    if (!nul) return false;
    if (out) out->str.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(nul - p));
    p = nul + 1;
    return true;
}

// Parse context: the decode context, the record's sig, and the stack of
// structs being parsed (so a union can select on an earlier field's value).
struct Frame {
    const TypeDef* type;
    const Value*   value;
};
struct ParseCtx {
    const SchemaSet&   s;
    const DecodeCtx&   ctx;
    uint32_t           recordSig;
    std::vector<Frame> frames;
    const Record*      rec  = nullptr;   // Member selectors: the record decoded so far
    uint32_t           inst = 0;         // ... and the scope the subrecord is placed in
};

// The nearest earlier field named `name` in the enclosing structs.
const Value* FieldByName(const ParseCtx& pc, const char* name) {
    for (size_t f = pc.frames.size(); f-- > 0;) {
        const TypeDef& t = *pc.frames[f].type;
        const Value&   v = *pc.frames[f].value;
        const Value*   hit = nullptr;
        uint16_t vi = 0;
        for (uint16_t i = 0; i < t.refCount && vi < v.items.size(); ++i) {
            const schema::FieldDef& fd = pc.s.fields[t.ref + i];
            if (fd.isBreak) continue;
            if (std::strcmp(fd.name, name) == 0) hit = &v.items[vi];
            ++vi;
        }
        if (hit) return hit;
    }
    return nullptr;
}

// The scalar a union's selector reads at this position.
bool SelectorValue(const ParseCtx& pc, const TypeDef& u, const uint8_t* p, const uint8_t* end,
                   uint64_t& out) {
    switch (u.selector) {
    case schema::Selector::Size:
        out = static_cast<uint64_t>(end - p);
        return true;
    case schema::Selector::FormVersion:
        out = pc.ctx.formVersion;
        return true;
    case schema::Selector::Record:
        out = pc.recordSig;
        return true;
    case schema::Selector::Member: {
        const Value* v = pc.rec ? pc.rec->LatestMemberValue(pc.inst, u.selField) : nullptr;
        if (!v) return false;
        out = v->bits;
        return true;
    }
    case schema::Selector::Field:
    case schema::Selector::Lookup: {
        const Value* v = FieldByName(pc, u.selField);
        if (!v) return false;
        if (u.selector == schema::Selector::Field) {
            out = u.selMask ? (v->bits & u.selMask) : v->bits;
            return true;
        }
        const schema::LookupDef& L = pc.s.lookups[u.lookup];
        const auto rows = pc.s.lookupRows.subspan(L.firstRow, L.rowCount);
        const uint32_t key = static_cast<uint32_t>(v->bits);
        const auto it = std::lower_bound(rows.begin(), rows.end(), key,
            [](const schema::LookupRow& r, uint32_t k) { return r.key < k; });
        if (it == rows.end() || it->key != key) return false;   // unknown key → default
        out = it->value;
        return true;
    }
    default:
        return false;
    }
}

// The case a union takes at this position, or -1 (refuse).
int ChooseCase(const ParseCtx& pc, const TypeDef& u, const uint8_t* p, const uint8_t* end) {
    uint64_t v = 0;
    const bool have = SelectorValue(pc, u, p, end, v);
    int dflt = -1;
    for (uint16_t i = 0; i < u.refCount; ++i) {
        const schema::CaseDef& c = pc.s.cases[u.ref + i];
        if (c.isDefault) { dflt = i; continue; }
        if (!have) continue;
        if (c.isRange) {
            const int64_t sv = static_cast<int64_t>(v);
            if (sv >= c.min && sv <= c.max) return i;
            continue;
        }
        for (uint16_t k = 0; k < c.valueCount; ++k)
            if (pc.s.caseValues[c.firstValue + k] == v) return i;
    }
    return dflt;
}

// Parses one value of `typeIdx` from [p, end) into `out`. On success `p` is
// advanced past exactly the bytes the layout owns.
bool Parse(ParseCtx& pc, uint16_t typeIdx, const uint8_t*& p, const uint8_t* end, Value& out) {
    const SchemaSet& s = pc.s;
    const TypeDef& t = s.types[typeIdx];
    if (const size_t w = ScalarWidth(t.kind)) return ReadLE(p, end, w, out.bits);
    switch (t.kind) {
    case Kind::ZString:
        return ReadZ(p, end, &out);
    case Kind::WString: {
        uint64_t len = 0;
        if (!ReadLE(p, end, 2, len)) return false;
        if (static_cast<uint64_t>(end - p) < len) return false;
        out.str.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(len));
        p += len;
        return true;
    }
    case Kind::LString:
        if (pc.ctx.localized) return ReadLE(p, end, 4, out.bits);
        return ReadZ(p, end, &out);
    case Kind::Bytes: {
        const size_t n = t.len == LenKind::Fixed ? t.count : static_cast<size_t>(end - p);
        if (static_cast<size_t>(end - p) < n) return false;
        out.bytes.assign(p, p + n);
        p += n;
        return true;
    }
    case Kind::Struct: {
        pc.frames.push_back(Frame{ &t, &out });
        out.items.reserve(t.refCount);
        uint16_t present = 0;
        bool ok = true, truncated = false;
        for (uint16_t i = 0; i < t.refCount; ++i) {
            const schema::FieldDef& f = s.fields[t.ref + i];
            if (f.isBreak) {
                if (p == end) { truncated = true; break; }   // a legitimate shorter form
                continue;
            }
            out.items.emplace_back();
            if (!Parse(pc, f.type, p, end, out.items.back())) { ok = false; break; }
            ++present;
        }
        pc.frames.pop_back();
        out.present = truncated ? present : Value::kAll;
        return ok;
    }
    case Kind::Array: {
        uint64_t count = 0;
        const size_t packed = schema::PackedElementWidth(s, t);
        switch (t.len) {
        case LenKind::Fixed:    count = t.count; break;
        case LenKind::Prefix8:  if (!ReadLE(p, end, 1, count)) return false; break;
        case LenKind::Prefix16: if (!ReadLE(p, end, 2, count)) return false; break;
        case LenKind::Prefix32: if (!ReadLE(p, end, 4, count)) return false; break;
        case LenKind::Rest:
            if (packed) {                          // whole elements to the end
                if (static_cast<size_t>(end - p) % packed != 0) return false;
                out.bytes.assign(p, end);
                p = end;
                return true;
            }
            while (p < end) {
                const uint8_t* before = p;
                out.items.emplace_back();
                if (!Parse(pc, t.ref, p, end, out.items.back())) return false;
                if (p == before) return false;     // zero-width element: no progress
            }
            return true;
        default: return false;
        }
        // Every element occupies at least one byte, so a count larger than the
        // bytes left is malformed: refuse it before looping.
        if (count > static_cast<uint64_t>(end - p)) return false;
        if (packed) {
            if (count * packed > static_cast<uint64_t>(end - p)) return false;
            out.bytes.assign(p, p + count * packed);
            p += count * packed;
            return true;
        }
        out.items.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            out.items.emplace_back();
            if (!Parse(pc, t.ref, p, end, out.items.back())) return false;
        }
        return true;
    }
    case Kind::Union: {
        const int c = ChooseCase(pc, t, p, end);
        if (c < 0) return false;                   // no case: refuse
        out.bits = static_cast<uint64_t>(c);
        const schema::CaseDef& cd = s.cases[t.ref + c];
        if (cd.empty) return true;
        out.items.emplace_back();
        return Parse(pc, cd.type, p, end, out.items.back());
    }
    default:
        return false;
    }
}

void Write(const SchemaSet& s, uint16_t typeIdx, const Value& v, const DecodeCtx& ctx,
           ByteWriter& w) {
    const TypeDef& t = s.types[typeIdx];
    if (const size_t n = ScalarWidth(t.kind)) {
        WriteLE(w, v.bits, n);
        return;
    }
    switch (t.kind) {
    case Kind::ZString:
        w.ZString(v.str);
        return;
    case Kind::WString:
        WriteLE(w, v.str.size(), 2);
        w.Bytes(v.str.data(), v.str.size());
        return;
    case Kind::LString:
        if (ctx.localized) WriteLE(w, v.bits, 4);
        else               w.ZString(v.str);
        return;
    case Kind::Bytes:
        if (t.len == LenKind::Fixed) {
            // A fixed-size field always emits exactly `count` bytes.
            for (uint32_t i = 0; i < t.count; ++i) w.U8(i < v.bytes.size() ? v.bytes[i] : 0);
        } else if (!v.bytes.empty()) {
            w.Bytes(v.bytes.data(), v.bytes.size());
        }
        return;
    case Kind::Struct: {
        uint16_t valueIndex = 0;
        const uint16_t limit = v.present == Value::kAll ? 0xFFFF : v.present;
        for (uint16_t i = 0; i < t.refCount; ++i) {
            const schema::FieldDef& f = s.fields[t.ref + i];
            if (f.isBreak) {
                if (valueIndex >= limit) return;   // re-emit the shorter form
                continue;
            }
            if (valueIndex >= v.items.size()) return;
            Write(s, f.type, v.items[valueIndex], ctx, w);
            ++valueIndex;
        }
        return;
    }
    case Kind::Array: {
        const size_t packed = schema::PackedElementWidth(s, t);
        const size_t count  = packed ? v.bytes.size() / packed : v.items.size();
        switch (t.len) {
        case LenKind::Prefix8:  WriteLE(w, count, 1); break;
        case LenKind::Prefix16: WriteLE(w, count, 2); break;
        case LenKind::Prefix32: WriteLE(w, count, 4); break;
        default: break;
        }
        if (packed) {
            // A fixed-count array always emits exactly `count` elements.
            const size_t want = t.len == LenKind::Fixed ? t.count * packed : count * packed;
            for (size_t i = 0; i < want; ++i) w.U8(i < v.bytes.size() ? v.bytes[i] : 0);
            return;
        }
        for (const Value& item : v.items) Write(s, t.ref, item, ctx, w);
        return;
    }
    case Kind::Union: {
        if (v.bits >= t.refCount) return;
        const schema::CaseDef& cd = s.cases[t.ref + static_cast<uint16_t>(v.bits)];
        if (!cd.empty && !v.items.empty()) Write(s, cd.type, v.items[0], ctx, w);
        return;
    }
    default:
        return;
    }
}

// A scope's entry signatures are sorted (schemagen), so this is a binary search.
int EntryLookup(const SchemaSet& s, const schema::GroupDef& g, uint32_t sig) {
    const schema::EntryDef* first = s.entries.data() + g.firstEntry;
    const schema::EntryDef* last  = first + g.entryCount;
    const schema::EntryDef* it = std::lower_bound(first, last, sig,
        [](const schema::EntryDef& e, uint32_t k) { return e.sig < k; });
    return (it != last && it->sig == sig) ? it->member : -1;
}

std::string SigText(uint32_t sig) {
    char buf[5] = {};
    std::memcpy(buf, &sig, 4);
    for (char& c : buf) if (c && (c < 0x20 || c > 0x7E)) c = '?';
    return buf;
}

// Visits every FormId-kind value under `v` (mutable).
template <class F>
void WalkFormIds(const SchemaSet& s, uint16_t typeIdx, Value& v, F&& f) {
    const TypeDef& t = s.types[typeIdx];
    switch (t.kind) {
    case Kind::FormId:
        f(v, t);
        return;
    case Kind::Struct: {
        uint16_t vi = 0;
        for (uint16_t i = 0; i < t.refCount && vi < v.items.size(); ++i) {
            const schema::FieldDef& fd = s.fields[t.ref + i];
            if (fd.isBreak) continue;
            WalkFormIds(s, fd.type, v.items[vi++], f);
        }
        return;
    }
    case Kind::Array:
        for (Value& item : v.items) WalkFormIds(s, t.ref, item, f);
        return;
    case Kind::Union:
        if (v.bits < t.refCount && !v.items.empty()) {
            const schema::CaseDef& cd = s.cases[t.ref + static_cast<uint16_t>(v.bits)];
            if (!cd.empty) WalkFormIds(s, cd.type, v.items[0], f);
        }
        return;
    default:
        return;
    }
}

// Visits every LString-kind value under `v` (mutable).
template <class F>
void WalkLStrings(const SchemaSet& s, uint16_t typeIdx, Value& v, F&& f) {
    const TypeDef& t = s.types[typeIdx];
    switch (t.kind) {
    case Kind::LString:
        f(v, t);
        return;
    case Kind::Struct: {
        uint16_t vi = 0;
        for (uint16_t i = 0; i < t.refCount && vi < v.items.size(); ++i) {
            const schema::FieldDef& fd = s.fields[t.ref + i];
            if (fd.isBreak) continue;
            WalkLStrings(s, fd.type, v.items[vi++], f);
        }
        return;
    }
    case Kind::Array:
        for (Value& item : v.items) WalkLStrings(s, t.ref, item, f);
        return;
    case Kind::Union:
        if (v.bits < t.refCount && !v.items.empty()) {
            const schema::CaseDef& cd = s.cases[t.ref + static_cast<uint16_t>(v.bits)];
            if (!cd.empty) WalkLStrings(s, cd.type, v.items[0], f);
        }
        return;
    default:
        return;
    }
}

// The first opaque `bytes` value under `v` that may hide FormIDs, if any.
bool HasUnsafeBytes(const SchemaSet& s, uint16_t typeIdx, const Value& v) {
    const TypeDef& t = s.types[typeIdx];
    if (t.flags & schema::kRemapUnsafe) return true;
    switch (t.kind) {
    case Kind::Bytes:
        return !v.bytes.empty() && !(t.flags & schema::kFormIdsNone);
    case Kind::Struct: {
        uint16_t vi = 0;
        for (uint16_t i = 0; i < t.refCount && vi < v.items.size(); ++i) {
            const schema::FieldDef& fd = s.fields[t.ref + i];
            if (fd.isBreak) continue;
            if (HasUnsafeBytes(s, fd.type, v.items[vi++])) return true;
        }
        return false;
    }
    case Kind::Array:
        if (schema::PackedElementWidth(s, t)) return false;   // plain: no FormIDs, nothing unsafe
        for (const Value& item : v.items)
            if (HasUnsafeBytes(s, t.ref, item)) return true;
        return false;
    case Kind::Union:
        if (v.bits < t.refCount && !v.items.empty()) {
            const schema::CaseDef& cd = s.cases[t.ref + static_cast<uint16_t>(v.bits)];
            if (!cd.empty) return HasUnsafeBytes(s, cd.type, v.items[0]);
        }
        return false;
    default:
        return false;
    }
}

} // namespace

// ── Decode / create / commit ─────────────────────────────────────────────────

Result<Record> Record::Decode(const SchemaSet& set, uint32_t sig, std::span<const uint8_t> payload,
                              const DecodeCtx& ctx) {
    const schema::RecordDef* def = set.Find(sig);
    if (!def) return Fail("no schema for record {}", SigText(sig));
    Record out;
    out.set_ = &set;
    out.def_ = def;
    out.ctx_ = ctx;
    out.bytes_.assign(payload.begin(), payload.end());
    out.insts_.push_back(Inst{ def->root, -1, 0 });

    // `fresh`: the instance was just opened by the subrecord being resolved, so
    // its opener may be placed in it (a later opener starts a new instance).
    struct Open { uint32_t inst; int32_t lastPos; bool fresh; };
    std::vector<Open> stack{ { 0, -1, false } };
    // Occurrences per (scope instance, member): subrecord nodes or group
    // instances. Flat: instance i's counters start at countBase[i].
    std::vector<uint32_t> counts(set.groups[def->root].memberCount, 0);
    std::vector<uint32_t> countBase{ 0 };
    auto count = [&](uint32_t inst, int member) -> uint32_t& {
        return counts[countBase[inst] + static_cast<uint32_t>(member)];
    };
    {
        size_t n = 0;
        SubIterator pre(out.bytes_);
        SubView s;
        while (pre.Next(s)) ++n;
        out.nodes_.reserve(n);
    }
    ParseCtx pc{ set, ctx, sig, {}, &out, 0 };

    SubIterator it(out.bytes_);
    SubView sub;
    while (it.Next(sub)) {
        Node n;
        n.sig     = sub.sig;
        n.off     = static_cast<uint32_t>(sub.data - out.bytes_.data());
        n.size    = sub.size;
        n.viaXxxx = sub.viaXxxx;

        bool placed = false;
        int level = static_cast<int>(stack.size()) - 1;
        while (level >= 0) {
            const uint32_t scopeInst = stack[static_cast<size_t>(level)].inst;
            const schema::GroupDef& g = out.GroupOf(scopeInst);
            int m;
            if (g.openSig == sub.sig && !stack[static_cast<size_t>(level)].fresh) {
                if (g.endSig != sub.sig) {
                    --level;                                     // reopens: a new instance outward
                    continue;
                }
                m = g.memberCount - 1;                           // same sig opens and ends: it ends here
            } else {
                m = EntryLookup(set, g, sub.sig);
            }
            if (m < 0) { --level; continue; }                    // not this scope
            const schema::MemberDef& md = set.members[g.firstMember + m];
            const int32_t lastPos = stack[static_cast<size_t>(level)].lastPos;
            if (g.ordered) {
                // The ordinal rule: a member at or before the last filled
                // position means this instance is complete.
                if (!(m > lastPos || (m == lastPos && md.repeat))) { --level; continue; }
            } else if (!md.repeat && count(scopeInst, m) > 0) {
                break;                                           // singular duplicate → raw
            }
            stack.resize(static_cast<size_t>(level) + 1);        // close inner scopes
            n.inst = scopeInst;
            if (md.isGroup) {
                const uint32_t ni = static_cast<uint32_t>(out.insts_.size());
                out.insts_.push_back(Inst{ md.group, static_cast<int32_t>(scopeInst),
                                           static_cast<uint16_t>(m) });
                ++count(scopeInst, m);
                countBase.push_back(static_cast<uint32_t>(counts.size()));
                counts.resize(counts.size() + set.groups[md.group].memberCount, 0);
                stack.back().lastPos = m;
                stack.back().fresh = false;
                stack.push_back(Open{ ni, -1, true });
                level = static_cast<int>(stack.size()) - 1;      // resolve inside it
                continue;
            }
            const uint8_t* p   = sub.data;
            const uint8_t* end = sub.data + sub.size;
            pc.inst = scopeInst;
            pc.frames.clear();
            Value v;
            if (Parse(pc, md.type, p, end, v) && p == end) {
                n.value   = std::move(v);
                n.decoded = true;
                n.member  = static_cast<int16_t>(m);
                ++count(scopeInst, m);
                stack.back().lastPos = m;
                stack.back().fresh = false;
                placed = true;
                if (g.endSig != 0 && g.endSig == sub.sig && m == g.memberCount - 1 &&
                    stack.size() > 1)
                    stack.pop_back();                            // the end member closes it
            }
            // else: refused — kept raw, in place, without advancing the position
            break;
        }
        if (!placed && level < 0) n.inst = stack.back().inst;     // unknown here: raw
        out.nodes_.push_back(std::move(n));
    }
    if (!it.ok()) return Fail("truncated/malformed subrecord stream in {}", SigText(sig));
    out.RebuildIndex();
    return out;
}

Result<Record> Record::Decode(const Plugin& plugin, RecordNode& node) {
    const auto payload = node.Payload();
    if (!payload) return std::unexpected(payload.error());
    DecodeCtx ctx;
    ctx.localized   = plugin.localized;
    ctx.formVersion = node.header.version;
    auto rec = Decode(schema::Main(), node.header.sig, *payload, ctx);
    if (!rec) return Wrap(rec.error(), "record {:08X}", node.header.formId);
    return rec;
}

Result<Record> Record::Create(const SchemaSet& set, uint32_t sig, const DecodeCtx& ctx) {
    return Decode(set, sig, {}, ctx);
}

Result<Record> Record::Create(const Plugin& plugin, uint32_t sig) {
    DecodeCtx ctx;
    ctx.localized   = plugin.localized;
    ctx.formVersion = 44;
    return Create(schema::Main(), sig, ctx);
}

Result<void> Record::Commit(Plugin& plugin, RecordNode& node, const Record& rec) {
    if (rec.Sig() != node.header.sig)
        return Fail("Commit: a {} record can't be written to {} {:08X}", SigText(rec.Sig()),
                    SigText(node.header.sig), node.header.formId);
    return SetRecordPayload(plugin, node, rec.Encode());
}

// ── Encode ───────────────────────────────────────────────────────────────────

void Record::Encode(ByteWriter& w, bool forceReencode) const {
    ByteWriter payload;
    for (const Node& n : nodes_) {
        if (n.member < 0 || (!n.dirty && !forceReencode)) {
            EmitSubrecord(w, n.sig, bytes_.data() + n.off, n.size, n.viaXxxx);
            continue;
        }
        payload.buf.clear();
        Write(*set_, MemberOf(n.inst, static_cast<uint16_t>(n.member)).type, NodeValue(n), ctx_,
              payload);
        EmitSubrecord(w, n.sig, payload.buf.data(), payload.buf.size(), n.viaXxxx);
    }
}

std::vector<uint8_t> Record::Encode(bool forceReencode) const {
    ByteWriter w;
    Encode(w, forceReencode);
    return std::move(w.buf);
}

size_t Record::RawCount() const {
    return static_cast<size_t>(std::count_if(nodes_.begin(), nodes_.end(),
                                              [](const Node& n) { return n.member < 0; }));
}

std::vector<uint32_t> Record::RawSigs() const {
    std::vector<uint32_t> out;
    for (const Node& n : nodes_)
        if (n.member < 0) out.push_back(n.sig);
    return out;
}

// ── Internals ────────────────────────────────────────────────────────────────

const schema::GroupDef& Record::GroupOf(uint32_t inst) const {
    return set_->groups[insts_[inst].group];
}

const schema::MemberDef& Record::MemberOf(uint32_t inst, uint16_t m) const {
    return set_->members[GroupOf(inst).firstMember + m];
}

const Value& Record::NodeValue(const Node& n) const {
    if (!n.decoded) {                 // decode parses eagerly; kept for safety
        const uint8_t* p = bytes_.data() + n.off;
        ParseCtx pc{ *set_, ctx_, def_->sig, {} };
        Parse(pc, MemberOf(n.inst, static_cast<uint16_t>(n.member)).type, p,
              bytes_.data() + n.off + n.size, n.value);
        n.decoded = true;
    }
    return n.value;
}

int32_t Record::ChildAtLevel(uint32_t nodeInst, uint32_t level) const {
    int32_t x = static_cast<int32_t>(nodeInst);
    while (x > 0) {
        if (insts_[static_cast<size_t>(x)].parent == static_cast<int32_t>(level)) return x;
        x = insts_[static_cast<size_t>(x)].parent;
    }
    return -1;
}

void Record::RebuildIndex() {
    const size_t ni = insts_.size();
    // Nodes by instance (counting sort keeps document order).
    nodeStart_.assign(ni + 1, 0);
    for (const Node& n : nodes_) ++nodeStart_[n.inst + 1];
    for (size_t i = 0; i < ni; ++i) nodeStart_[i + 1] += nodeStart_[i];
    nodeIdx_.resize(nodes_.size());
    std::vector<uint32_t> fill(nodeStart_.begin(), nodeStart_.end() - 1);
    for (uint32_t k = 0; k < nodes_.size(); ++k) nodeIdx_[fill[nodes_[k].inst]++] = k;

    // Child instances in document order: where each first holds a node (its
    // own or a descendant's), then instances with no nodes yet.
    std::vector<uint32_t> order;
    order.reserve(ni);
    std::vector<uint8_t> seen(ni, 0);
    for (const Node& n : nodes_)
        for (uint32_t c = n.inst; c != 0 && !seen[c]; c = static_cast<uint32_t>(insts_[c].parent)) {
            seen[c] = 1;
            order.push_back(c);
        }
    for (uint32_t i = 1; i < ni; ++i)
        if (!seen[i]) order.push_back(i);
    kidStart_.assign(ni + 1, 0);
    for (uint32_t c : order) ++kidStart_[static_cast<size_t>(insts_[c].parent) + 1];
    for (size_t i = 0; i < ni; ++i) kidStart_[i + 1] += kidStart_[i];
    kidIdx_.resize(order.size());
    fill.assign(kidStart_.begin(), kidStart_.end() - 1);
    for (uint32_t c : order) kidIdx_[fill[static_cast<size_t>(insts_[c].parent)]++] = c;
}

std::vector<uint32_t> Record::InstancesOf(uint32_t parent, uint16_t slot) const {
    std::vector<uint32_t> out;
    if (static_cast<size_t>(parent) + 1 >= kidStart_.size()) return out;   // created since the index
    for (uint32_t k = kidStart_[parent]; k < kidStart_[parent + 1]; ++k)
        if (insts_[kidIdx_[k]].slot == slot) out.push_back(kidIdx_[k]);
    return out;
}

int32_t Record::FirstNode(uint32_t inst, uint16_t member) const {
    return NthNode(inst, member, 0);
}

int32_t Record::NthNode(uint32_t inst, uint16_t member, size_t n) const {
    if (static_cast<size_t>(inst) + 1 >= nodeStart_.size()) return -1;  // created since the index
    for (uint32_t k = nodeStart_[inst]; k < nodeStart_[inst + 1]; ++k)
        if (nodes_[nodeIdx_[k]].member == static_cast<int16_t>(member) && n-- == 0)
            return static_cast<int32_t>(nodeIdx_[k]);
    return -1;
}

size_t Record::InsertPos(uint32_t inst, uint16_t slot) const {
    int64_t best = -1, firstGreater = -1;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        int s;
        if (n.inst == inst) {
            s = n.member;                         // raw nodes (-1) sort first
        } else {
            const int32_t c = ChildAtLevel(n.inst, inst);
            if (c < 0) continue;                  // outside this instance
            s = insts_[static_cast<size_t>(c)].slot;
        }
        if (s <= static_cast<int>(slot)) best = static_cast<int64_t>(i);
        else if (firstGreater < 0) firstGreater = static_cast<int64_t>(i);
    }
    if (best >= 0) return static_cast<size_t>(best) + 1;
    if (firstGreater >= 0) return static_cast<size_t>(firstGreater);
    if (inst == 0) return nodes_.size();
    return InsertPos(static_cast<uint32_t>(insts_[inst].parent), insts_[inst].slot);
}

int32_t Record::WalkGroupsConst(uint32_t inst, const RelPath& p) const {
    if (p.memberDepth == 0) return -1;
    uint32_t cur = inst;
    for (uint8_t d = 0; d + 1 < p.memberDepth; ++d) {
        const std::vector<uint32_t> v = InstancesOf(cur, p.idx[d]);
        if (v.empty()) return -1;
        cur = v.front();
    }
    return static_cast<int32_t>(cur);
}

int32_t Record::WalkGroups(uint32_t inst, const RelPath& p, bool create) {
    if (p.memberDepth == 0) return -1;
    uint32_t cur = inst;
    bool created = false;
    for (uint8_t d = 0; d + 1 < p.memberDepth; ++d) {
        const std::vector<uint32_t> v = InstancesOf(cur, p.idx[d]);
        if (!v.empty()) {
            cur = v.front();
            continue;
        }
        if (!create) return -1;
        const schema::MemberDef& md = MemberOf(cur, p.idx[d]);
        insts_.push_back(Inst{ md.group, static_cast<int32_t>(cur), p.idx[d] });
        cur = static_cast<uint32_t>(insts_.size() - 1);
        created = true;
    }
    if (created) RebuildIndex();
    return static_cast<int32_t>(cur);
}

const TypeDef& Record::FieldType(const TypeDef& structType, uint16_t valueIndex) const {
    uint16_t vi = 0;
    for (uint16_t i = 0; i < structType.refCount; ++i) {
        const schema::FieldDef& f = set_->fields[structType.ref + i];
        if (f.isBreak) continue;
        if (vi++ == valueIndex) return set_->types[f.type];
    }
    return set_->types[0];   // unreachable for generated paths
}

const TypeDef* Record::TypeAt(uint32_t inst, const RelPath& p) const {
    if (p.memberDepth == 0) return nullptr;
    const schema::GroupDef* g = &GroupOf(inst);
    const TypeDef* t = nullptr;
    for (uint8_t d = 0; d < p.memberDepth; ++d) {
        const schema::MemberDef& md = set_->members[g->firstMember + p.idx[d]];
        if (md.isGroup) {
            if (d + 1 == p.memberDepth) return nullptr;    // a group, not a value
            g = &set_->groups[md.group];
        } else {
            if (d + 1 != p.memberDepth) return nullptr;
            t = &set_->types[md.type];
        }
    }
    for (uint8_t d = p.memberDepth; d < p.depth && t; ++d) {
        if (t->kind == Kind::Struct) {
            t = &FieldType(*t, p.idx[d]);
        } else if (t->kind == Kind::Array) {
            t = &set_->types[t->ref];                       // an element index
        } else if (t->kind == Kind::Union) {
            if (p.idx[d] >= t->refCount) return nullptr;
            const schema::CaseDef& cd = set_->cases[t->ref + p.idx[d]];
            if (cd.empty) return nullptr;
            t = &set_->types[cd.type];
        } else {
            return nullptr;
        }
    }
    return t;
}

const Value* Record::Descend(const Value& root, uint16_t type, const RelPath& p, Value* scratch) const {
    const Value* v = &root;
    const TypeDef* t = &set_->types[type];
    for (uint8_t d = p.memberDepth; d < p.depth; ++d) {
        const uint16_t i = p.idx[d];
        if (t->kind == Kind::Union) {
            if (v->bits != i || v->items.empty()) return nullptr;     // other case taken
            t = &set_->types[set_->cases[t->ref + i].type];
            v = &v->items[0];
            continue;
        }
        if (t->kind == Kind::Array) {
            if (schema::PackedElementWidth(*set_, *t)) {
                size_t off = 0;
                uint16_t part = 0;
                if (!scratch || !PackedOffset(*v, *t, p, d, off, part)) return nullptr;
                *scratch = Unpack(*set_, part, v->bytes.data() + off);
                return scratch;
            }
            if (i >= v->items.size()) return nullptr;
            t = &set_->types[t->ref];
            v = &v->items[i];
            continue;
        }
        if (t->kind != Kind::Struct) return nullptr;
        if (v->present != Value::kAll && i >= v->present) return nullptr;
        if (i >= v->items.size()) return nullptr;
        t = &FieldType(*t, i);
        v = &v->items[i];
    }
    return v;
}

bool Record::PackedOffset(const Value& arr, const TypeDef& arrType, const RelPath& p, uint8_t d,
                          size_t& off, uint16_t& type) const {
    const TypeDef* t = &arrType;
    off = 0;
    for (; d < p.depth; ++d) {
        const uint16_t i = p.idx[d];
        if (t->kind == Kind::Array) {
            const size_t w = set_->types[t->ref].plainSize;
            const size_t n = t == &arrType ? arr.bytes.size() / w : t->count;
            if (w == 0 || i >= n) return false;
            off += i * w;
            t = &set_->types[t->ref];
        } else if (t->kind == Kind::Struct) {
            if (i >= t->refCount) return false;
            for (uint16_t k = 0; k < i; ++k) off += FieldType(*t, k).plainSize;
            t = &FieldType(*t, i);
        } else {
            return false;
        }
    }
    type = static_cast<uint16_t>(t - set_->types.data());
    return off + t->plainSize <= arr.bytes.size();
}

Value Record::Unpack(const SchemaSet& s, uint16_t type, const uint8_t* p) {
    const TypeDef& t = s.types[type];
    Value v;
    if (const size_t w = ScalarWidth(t.kind)) {
        for (size_t b = 0; b < w; ++b) v.bits |= static_cast<uint64_t>(p[b]) << (8 * b);
    } else if (t.kind == Kind::Array) {
        v.bytes.assign(p, p + t.plainSize);                       // nested arrays stay packed
    } else if (t.kind == Kind::Struct) {
        v.items.reserve(t.refCount);
        size_t off = 0;
        for (uint16_t i = 0; i < t.refCount; ++i) {
            const uint16_t ft = s.fields[t.ref + i].type;
            v.items.push_back(Unpack(s, ft, p + off));
            off += s.types[ft].plainSize;
        }
    }
    return v;
}

void Record::Pack(const SchemaSet& s, uint16_t type, const Value& v, uint8_t* out) {
    const TypeDef& t = s.types[type];
    if (const size_t w = ScalarWidth(t.kind)) {
        for (size_t b = 0; b < w; ++b) out[b] = static_cast<uint8_t>(v.bits >> (8 * b));
    } else if (t.kind == Kind::Array) {
        for (size_t b = 0; b < t.plainSize; ++b) out[b] = b < v.bytes.size() ? v.bytes[b] : 0;
    } else if (t.kind == Kind::Struct) {
        size_t off = 0;
        for (uint16_t i = 0; i < t.refCount; ++i) {
            const uint16_t ft = s.fields[t.ref + i].type;
            if (i < v.items.size()) Pack(s, ft, v.items[i], out + off);
            else std::memset(out + off, 0, s.types[ft].plainSize);
            off += s.types[ft].plainSize;
        }
    }
}

const Value* Record::LatestMemberValue(uint32_t inst, std::string_view name) const {
    for (int32_t cur = static_cast<int32_t>(inst); cur >= 0;
         cur = insts_[static_cast<size_t>(cur)].parent) {
        const schema::GroupDef& g = GroupOf(static_cast<uint32_t>(cur));
        int m = -1;
        for (uint16_t i = 0; i < g.memberCount && m < 0; ++i) {
            const schema::MemberDef& md = set_->members[g.firstMember + i];
            if (!md.isGroup && name == md.name) m = i;
        }
        if (m < 0) continue;
        for (size_t k = nodes_.size(); k-- > 0;)
            if (nodes_[k].inst == static_cast<uint32_t>(cur) && nodes_[k].member == m)
                return &NodeValue(nodes_[k]);
    }
    return nullptr;
}

const Value* Record::FindValue(uint32_t inst, const RelPath& p, Value* scratch) const {
    const int32_t scope = WalkGroupsConst(inst, p);
    if (scope < 0) return nullptr;
    const int32_t ni = NthNode(static_cast<uint32_t>(scope), p.idx[p.memberDepth - 1], p.occurrence);
    if (ni < 0) return nullptr;
    return Descend(NodeValue(nodes_[static_cast<size_t>(ni)]),
                   MemberOf(static_cast<uint32_t>(scope), p.idx[p.memberDepth - 1]).type, p, scratch);
}

std::vector<const Value*> Record::FindAllValues(uint32_t inst, const RelPath& p,
                                                std::deque<Value>* scratch) const {
    std::vector<const Value*> out;
    const int32_t scope = WalkGroupsConst(inst, p);
    if (scope < 0) return out;
    const uint16_t m = p.idx[p.memberDepth - 1];
    const uint16_t type = MemberOf(static_cast<uint32_t>(scope), m).type;
    for (size_t k = 0;; ++k) {
        const int32_t ni = NthNode(static_cast<uint32_t>(scope), m, k);
        if (ni < 0) break;
        Value* slot = nullptr;
        if (scratch) slot = &scratch->emplace_back();
        if (const Value* v = Descend(NodeValue(nodes_[static_cast<size_t>(ni)]), type, p, slot)) out.push_back(v);
    }
    return out;
}

Value Record::DefaultValue(uint16_t typeIdx) const {
    const TypeDef& t = set_->types[typeIdx];
    Value v;
    switch (t.kind) {
    case Kind::Bytes:
        if (t.len == LenKind::Fixed) v.bytes.assign(t.count, 0);
        break;
    case Kind::Struct:
        for (uint16_t i = 0; i < t.refCount; ++i) {
            const schema::FieldDef& f = set_->fields[t.ref + i];
            if (!f.isBreak) v.items.push_back(DefaultValue(f.type));
        }
        break;
    case Kind::Array:
        if (t.len == LenKind::Fixed) {
            if (const size_t w = schema::PackedElementWidth(*set_, t)) v.bytes.assign(t.count * w, 0);
            else for (uint32_t i = 0; i < t.count; ++i) v.items.push_back(DefaultValue(t.ref));
        }
        break;
    case Kind::Union: {
        // The default case if there is one, otherwise the first.
        uint16_t pick = 0;
        for (uint16_t i = 0; i < t.refCount; ++i)
            if (set_->cases[t.ref + i].isDefault) pick = i;
        v.bits = pick;
        const schema::CaseDef& cd = set_->cases[t.ref + pick];
        if (!cd.empty) v.items.push_back(DefaultValue(cd.type));
        break;
    }
    default:
        break;
    }
    return v;
}

void Record::StoreValue(uint32_t inst, const RelPath& p, Value v) {
    if (!TypeAt(inst, p)) return;
    const int32_t scope = WalkGroups(inst, p, true);
    if (scope < 0) return;
    const uint32_t s = static_cast<uint32_t>(scope);
    const uint16_t m = p.idx[p.memberDepth - 1];
    const schema::MemberDef& md = MemberOf(s, m);
    const int32_t ni = NthNode(s, m, p.occurrence);
    if (ni < 0 && p.occurrence > 0) return;      // only the first occurrence is created on write

    Value whole = ni >= 0 ? NodeValue(nodes_[static_cast<size_t>(ni)]) : DefaultValue(md.type);
    if (p.depth == p.memberDepth) {
        whole = std::move(v);
    } else {
        bool packedWrite = false;
        Value* cur = &whole;
        const TypeDef* t = &set_->types[md.type];
        for (uint8_t d = p.memberDepth; d < p.depth; ++d) {
            const uint16_t i = p.idx[d];
            if (t->kind == Kind::Union) {
                if (i >= t->refCount) return;
                const schema::CaseDef& cd = set_->cases[t->ref + i];
                if (cd.empty) return;
                if (cur->bits != i || cur->items.empty()) {   // switch to this case
                    cur->bits = i;
                    cur->items.assign(1, DefaultValue(cd.type));
                }
                cur = &cur->items[0];
                t = &set_->types[cd.type];
                continue;
            }
            if (t->kind == Kind::Array) {
                if (schema::PackedElementWidth(*set_, *t)) {
                    size_t off = 0;
                    uint16_t type = 0;
                    if (!PackedOffset(*cur, *t, p, d, off, type)) return;
                    Pack(*set_, type, v, cur->bytes.data() + off);
                    packedWrite = true;
                    break;
                }
                if (i >= cur->items.size()) return;
                cur = &cur->items[i];
                t = &set_->types[t->ref];
                continue;
            }
            if (cur->present != Value::kAll && i >= cur->present) {
                // Setting a field past a break extends the struct to its full form.
                const Value full = DefaultValue(static_cast<uint16_t>(t - set_->types.data()));
                for (size_t k = cur->items.size(); k < full.items.size(); ++k)
                    cur->items.push_back(full.items[k]);
                cur->present = Value::kAll;
            }
            if (i >= cur->items.size()) return;
            const TypeDef& ft = FieldType(*t, i);
            cur = &cur->items[i];
            t = &ft;
        }
        if (!packedWrite) *cur = std::move(v);
    }

    if (ni >= 0) {
        Node& n = nodes_[static_cast<size_t>(ni)];
        if (NodeValue(n) == whole) return;       // unchanged: stays verbatim
        n.value = std::move(whole);
        n.dirty = true;
    } else {
        Node n;
        n.sig     = md.sig;
        n.inst    = s;
        n.member  = static_cast<int16_t>(m);
        n.dirty   = true;
        n.decoded = true;
        n.value   = std::move(whole);
        const size_t pos = InsertPos(s, m);
        nodes_.insert(nodes_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(n));
        RebuildIndex();
    }
    UpdateCount(s, m);
}

void Record::StoreAll(uint32_t inst, const RelPath& p, std::vector<Value> vs) {
    if (p.depth != p.memberDepth || !TypeAt(inst, p)) return;
    const int32_t scope = WalkGroups(inst, p, !vs.empty());
    if (scope < 0) return;
    const uint32_t s = static_cast<uint32_t>(scope);
    const uint16_t m = p.idx[p.memberDepth - 1];

    std::vector<size_t> have;
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].inst == s && nodes_[i].member == static_cast<int16_t>(m)) have.push_back(i);

    const size_t keep = std::min(have.size(), vs.size());
    for (size_t i = 0; i < keep; ++i) {
        Node& n = nodes_[have[i]];
        if (NodeValue(n) == vs[i]) continue;
        n.value = std::move(vs[i]);
        n.dirty = true;
    }
    for (size_t i = have.size(); i-- > keep;)   // drop extras, back to front
        nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(have[i]));

    size_t pos = keep > 0 ? have[keep - 1] + 1 : InsertPos(s, m);
    const schema::MemberDef& md = MemberOf(s, m);
    for (size_t i = keep; i < vs.size(); ++i) {
        Node n;
        n.sig     = md.sig;
        n.inst    = s;
        n.member  = static_cast<int16_t>(m);
        n.dirty   = true;
        n.decoded = true;
        n.value   = std::move(vs[i]);
        nodes_.insert(nodes_.begin() + static_cast<std::ptrdiff_t>(pos++), std::move(n));
    }
    if (have.size() != vs.size()) RebuildIndex();
    UpdateCount(s, m);
}

void Record::ClearAt(uint32_t inst, const RelPath& p) {
    if (p.memberDepth == 0 || p.depth != p.memberDepth) return;   // members only
    const int32_t scope = WalkGroupsConst(inst, p);
    if (scope < 0) return;
    const uint16_t m = p.idx[p.memberDepth - 1];
    if (MemberOf(static_cast<uint32_t>(scope), m).isGroup) return;
    std::erase_if(nodes_, [&](const Node& n) {
        return n.inst == static_cast<uint32_t>(scope) && n.member == static_cast<int16_t>(m);
    });
    RebuildIndex();
    UpdateCount(static_cast<uint32_t>(scope), m);
}

size_t Record::CountAt(uint32_t inst, const RelPath& p) const {
    if (p.memberDepth == 0) return 0;
    const int32_t scope = WalkGroupsConst(inst, p);
    if (scope < 0) return 0;
    const uint16_t m = p.idx[p.memberDepth - 1];
    size_t n = 0;
    for (const Node& node : nodes_)
        n += node.inst == static_cast<uint32_t>(scope) && node.member == static_cast<int16_t>(m);
    return n;
}

size_t Record::AppendAt(uint32_t inst, const RelPath& p) {
    if (!TypeAt(inst, RelPath{ p.idx, p.memberDepth, p.memberDepth })) return 0;
    const int32_t scope = WalkGroups(inst, p, true);
    if (scope < 0) return 0;
    const uint32_t s = static_cast<uint32_t>(scope);
    const uint16_t m = p.idx[p.memberDepth - 1];
    const schema::MemberDef& md = MemberOf(s, m);
    size_t count = 0, pos = InsertPos(s, m);
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].inst == s && nodes_[i].member == static_cast<int16_t>(m)) { ++count; pos = i + 1; }
    if (count > 0 && !md.repeat) return 0;       // a singular member has one occurrence
    Node n;
    n.sig     = md.sig;
    n.inst    = s;
    n.member  = static_cast<int16_t>(m);
    n.dirty   = true;
    n.decoded = true;
    n.value   = DefaultValue(md.type);
    nodes_.insert(nodes_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(n));
    RebuildIndex();
    UpdateCount(s, m);
    return count;
}

void Record::ResizeAt(uint32_t inst, const RelPath& p, size_t n) {
    const TypeDef* t = TypeAt(inst, p);
    if (!t || t->kind != Kind::Array || t->len == LenKind::Fixed) return;
    const Value* have = FindValue(inst, p);
    Value v = have ? *have : Value{};
    if (const size_t w = schema::PackedElementWidth(*set_, *t)) {
        v.bytes.resize(n * w, 0);
    } else {
        if (v.items.size() > n) v.items.resize(n);
        while (v.items.size() < n) v.items.push_back(DefaultValue(t->ref));
    }
    StoreValue(inst, p, std::move(v));
}

void Record::UpdateCount(uint32_t inst, uint16_t member) {
    const schema::GroupDef& g = GroupOf(inst);
    for (uint16_t c = 0; c < g.memberCount; ++c) {
        const schema::MemberDef& cd = set_->members[g.firstMember + c];
        if (cd.countOf != static_cast<int16_t>(member)) continue;
        const schema::MemberDef& md = MemberOf(inst, member);
        std::optional<uint64_t> count;
        if (md.repeat) {
            uint64_t k = 0;
            for (const Node& n : nodes_)
                if (n.inst == inst && n.member == static_cast<int16_t>(member)) ++k;
            if (k) count = k;
        } else if (const int32_t ni = FirstNode(inst, member); ni >= 0) {
            const Value& v = NodeValue(nodes_[static_cast<size_t>(ni)]);
            const TypeDef& mt = set_->types[md.type];
            const size_t w = schema::PackedElementWidth(*set_, mt);
            count = mt.kind == Kind::Bytes ? v.bytes.size() : w ? v.bytes.size() / w : v.items.size();
        }
        RelPath cp;
        cp.idx[0] = c;
        cp.memberDepth = cp.depth = 1;
        if (!count) {
            ClearAt(inst, cp);
        } else {
            Value cv;
            cv.bits = *count;
            StoreValue(inst, cp, std::move(cv));
        }
    }
}

std::vector<Cursor> Record::GroupsAt(uint32_t inst, const RelPath& p) {
    std::vector<Cursor> out;
    if (p.memberDepth == 0) return out;
    const int32_t scope = WalkGroupsConst(inst, p);
    if (scope < 0) return out;
    const uint16_t m = p.idx[p.memberDepth - 1];
    if (!MemberOf(static_cast<uint32_t>(scope), m).isGroup) return out;
    for (uint32_t i : InstancesOf(static_cast<uint32_t>(scope), m)) out.push_back(Cursor{ this, i });
    return out;
}

Cursor Record::EnsureGroupAt(uint32_t inst, const RelPath& p) {
    const int32_t parent = WalkGroups(inst, p, true);
    if (parent < 0) return Cursor{ this, inst };
    const uint16_t m = p.idx[p.memberDepth - 1];
    const std::vector<uint32_t> v = InstancesOf(static_cast<uint32_t>(parent), m);
    if (!v.empty()) return Cursor{ this, v.front() };
    return AddGroupAt(inst, p);
}

Cursor Record::AddGroupAt(uint32_t inst, const RelPath& p) {
    const int32_t parent = WalkGroups(inst, p, true);
    if (parent < 0) return Cursor{ this, inst };
    const uint16_t m = p.idx[p.memberDepth - 1];
    const schema::MemberDef& md = MemberOf(static_cast<uint32_t>(parent), m);
    if (!md.isGroup) return Cursor{ this, inst };
    insts_.push_back(Inst{ md.group, parent, m });
    RebuildIndex();
    return Cursor{ this, static_cast<uint32_t>(insts_.size() - 1) };
}

// ── String paths ─────────────────────────────────────────────────────────────

std::optional<RelPath> Record::Find(std::string_view path) const {
    RelPath out;
    const schema::GroupDef* g = &set_->groups[def_->root];
    const TypeDef* t = nullptr;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string_view seg =
            path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (out.depth >= out.idx.size() || seg.empty()) return std::nullopt;
        bool found = false;
        if (!t) {                                       // member names
            for (uint16_t i = 0; i < g->memberCount; ++i) {
                const schema::MemberDef& md = set_->members[g->firstMember + i];
                if (seg != md.name) continue;
                out.idx[out.depth++] = i;
                out.memberDepth = out.depth;
                if (md.isGroup) g = &set_->groups[md.group];
                else            t = &set_->types[md.type];
                found = true;
                break;
            }
        } else if (t->kind == Kind::Struct) {           // struct field names
            uint16_t vi = 0;
            for (uint16_t i = 0; i < t->refCount; ++i) {
                const schema::FieldDef& f = set_->fields[t->ref + i];
                if (f.isBreak) continue;
                if (seg == f.name) {
                    out.idx[out.depth++] = vi;
                    t = &set_->types[f.type];
                    found = true;
                    break;
                }
                ++vi;
            }
        }
        if (!found) return std::nullopt;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return out;
}

// ── FormIDs ──────────────────────────────────────────────────────────────────

Result<void> Record::CheckRemapSafe() const {
    for (const Node& n : nodes_) {
        if (n.member < 0)
            return Fail("raw subrecord {} may hold FormIDs", SigText(n.sig));
        const schema::MemberDef& md = MemberOf(n.inst, static_cast<uint16_t>(n.member));
        if (HasUnsafeBytes(*set_, md.type, NodeValue(n)))
            return Fail("opaque bytes in {} may hold FormIDs", SigText(n.sig));
    }
    return {};
}

Result<void> Record::VisitFormIds(const std::function<void(FormId&)>& fn) {
    if (auto safe = CheckRemapSafe(); !safe) return safe;
    for (Node& n : nodes_) {
        const uint16_t type = MemberOf(n.inst, static_cast<uint16_t>(n.member)).type;
        Value v = NodeValue(n);
        WalkFormIds(*set_, type, v, [&](Value& id, const TypeDef&) {
            FormId f{ static_cast<uint32_t>(id.bits) };
            fn(f);
            id.bits = f.raw;
        });
        if (!(v == n.value)) {
            n.value = std::move(v);
            n.dirty = true;
        }
    }
    return {};
}

Result<void> Record::Relocalize(
    bool localized,
    const std::function<std::optional<std::string>(uint32_t, schema::StrTable)>& lookup) {
    if (localized == ctx_.localized) return {};
    for (const Node& n : nodes_) {
        if (n.member < 0)
            return Fail("raw subrecord {} may hold string ids", SigText(n.sig));
    }
    // Convert every node first; apply only if all of them convert.
    std::vector<std::pair<size_t, Value>> changed;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        Value v = NodeValue(n);
        bool any = false, ok = true;
        WalkLStrings(*set_, MemberOf(n.inst, static_cast<uint16_t>(n.member)).type, v,
                     [&](Value& s, const TypeDef& t) {
            any = true;
            if (!ok) return;
            if (localized) { ok = false; return; }
            const std::optional<std::string> text = lookup(static_cast<uint32_t>(s.bits), t.table);
            if (!text) { ok = false; return; }
            s.str  = *text;
            s.bits = 0;
        });
        if (!ok) {
            if (localized)
                return Fail("can't move inline strings in {} into a localized plugin", SigText(n.sig));
            return Fail("a string id in {} isn't in the string tables", SigText(n.sig));
        }
        if (any) changed.emplace_back(i, std::move(v));
    }
    for (auto& [i, v] : changed) {
        nodes_[i].value = std::move(v);
        nodes_[i].dirty = true;
    }
    ctx_.localized = localized;
    return {};
}

void Record::ForEachFormId(
    const std::function<void(FormId, std::span<const uint32_t>)>& fn) const {
    for (const Node& n : nodes_) {
        if (n.member < 0) continue;
        const uint16_t type = MemberOf(n.inst, static_cast<uint16_t>(n.member)).type;
        Value v = NodeValue(n);
        WalkFormIds(*set_, type, v, [&](Value& id, const TypeDef& t) {
            fn(FormId{ static_cast<uint32_t>(id.bits) }, set_->links.subspan(t.linkFirst, t.linkCount));
        });
    }
}

} // namespace esper
