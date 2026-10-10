#define _CRT_SECURE_NO_WARNINGS

#include <ESPER/Writer.h>

#include <ESPER/ByteWriter.h>
#include <ESPER/CaseUtil.h>
#include <ESPER/PluginFlags.h>
#include <ESPER/Record.h>
#include <ESPER/RecordHeader.h>
#include <ESPER/Subrecords.h>

#include <cstdio>
#include <cstring>

namespace esper {

// ── Serialization ───────────────────────────────────────────────────────────

static void EmitRecord(ByteWriter& w, const RecordNode& r) {
    RecHeader h = r.header;
    const uint8_t* data;
    uint32_t       size;
    if (r.modified) {
        data = r.modified->data();
        size = static_cast<uint32_t>(r.modified->size());
    } else {
        data = r.raw;
        size = r.rawSize;
    }
    h.dataSize = size;
    w.Bytes(&h, sizeof(h));
    if (size) w.Bytes(data, size);
}

static void EmitNode(ByteWriter& w, const Node& n) {
    if (n.IsGroup()) {
        const GroupNode& g      = *n.group;
        const size_t     hdrOff = w.Tell();
        w.Bytes(&g.header, sizeof(g.header));   // groupSize patched below
        for (const Node& c : g.children) EmitNode(w, c);
        w.PatchU32(hdrOff + offsetof(GrupHeader, groupSize),
                   static_cast<uint32_t>(w.Tell() - hdrOff));
    } else {
        EmitRecord(w, n.rec);
    }
}

Result<std::vector<uint8_t>> SerializePlugin(const Plugin& p) {
    if (p.tes4.raw == nullptr && !p.tes4.modified)
        return Fail("SerializePlugin: plugin '{}' has no TES4 record (not loaded and not finalized)",
                    p.filename);

    ByteWriter w;
    w.buf.reserve(p.buffer.size() + 4096);

    EmitRecord(w, p.tes4);
    for (const Node& n : p.tree) EmitNode(w, n);

    return std::move(w.buf);
}

Result<void> SavePlugin(Plugin& p, const std::string& path) {
    if (p.edited) {
        if (auto r = FinalizePlugin(p); !r) return r;
    }
    auto bytes = SerializePlugin(p);
    if (!bytes) return std::unexpected(bytes.error());

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return Fail("cannot open for write: {}", path);
    const size_t written =
        bytes->empty() ? 0 : std::fwrite(bytes->data(), 1, bytes->size(), f);
    const bool ok = (written == bytes->size()) && (std::fclose(f) == 0);
    if (!ok) return Fail("short write: {}", path);
    return {};
}

// ── Mutation ────────────────────────────────────────────────────────────────

void CreateEmptyPlugin(Plugin& p, const std::string& filename, bool light) {
    p = Plugin{};
    p.filename = filename;
    p.isLight  = light;
    p.edited   = true;

    p.hedrVersion      = 1.71f;
    p.hedrNumRecords   = 0;
    p.hedrNextObjectId = 0x800u;

    p.tes4.header         = RecHeader{};
    p.tes4.header.sig     = sig::TES4;
    p.tes4.header.version = 44;
    // Payload synthesized by FinalizePlugin.
}

Result<uint32_t> AllocateFormId(Plugin& p) {
    if (p.masters.size() > 0xFDu)
        return Fail("AllocateFormId: {} masters exceeds the format limit", p.masters.size());
    if (p.hedrNextObjectId < 0x800u) p.hedrNextObjectId = 0x800u;

    const uint32_t objectId = p.hedrNextObjectId;
    if (p.isLight && objectId > 0xFFFu)
        return Fail("AllocateFormId: ESL object-id space exhausted (next 0x{:X} > 0xFFF)", objectId);
    if (objectId > 0x00FFFFFFu)
        return Fail("AllocateFormId: object-id space exhausted");
    ++p.hedrNextObjectId;
    p.edited = true;
    return (static_cast<uint32_t>(p.masters.size()) << 24) | objectId;
}

static void IndexTreeLevel(Plugin& p, std::vector<Node>& nodes) {
    for (Node& n : nodes) {
        if (n.IsGroup()) IndexTreeLevel(p, n.group->children);
        else             p.index.insert_or_assign(n.rec.header.formId, &n.rec);
    }
}

void RebuildIndex(Plugin& p) {
    p.index.clear();
    IndexTreeLevel(p, p.tree);
}

Result<void> SetRecordPayload(Plugin& p, RecordNode& node, std::vector<uint8_t> payload) {
    if (node.Compressed()) {
        auto zr = ZlibDeflate(payload);
        if (!zr) return Wrap(zr.error(), "record {:08X}", node.header.formId);
        const std::vector<uint8_t>& z = *zr;
        auto disk = std::make_unique<std::vector<uint8_t>>();
        disk->reserve(4 + z.size());
        const uint32_t logicalSize = static_cast<uint32_t>(payload.size());
        disk->insert(disk->end(),
                     reinterpret_cast<const uint8_t*>(&logicalSize),
                     reinterpret_cast<const uint8_t*>(&logicalSize) + 4);
        disk->insert(disk->end(), z.begin(), z.end());
        node.modified = std::move(disk);
        node.inflated = std::make_unique<std::vector<uint8_t>>(std::move(payload));
    } else {
        node.modified = std::make_unique<std::vector<uint8_t>>(std::move(payload));
        node.inflated.reset();
    }
    node.dirty = true;
    p.edited   = true;
    return {};
}

namespace {
struct SelfIndexBump {
    uint32_t oldSelf;
    uint32_t newSelf;
    // Returns true when it changed `id`.
    bool Apply(uint32_t& id) const {
        // A null FormID (0) is "no reference", not object 0 of master 0 —
        // it must never be renumbered (a masterless plugin has oldSelf == 0,
        // which would otherwise capture every null ref).
        if (id == 0) return false;
        if ((id >> 24) != oldSelf) return false;
        id = (newSelf << 24) | (id & 0x00FFFFFFu);
        return true;
    }
};
} // namespace

Result<int> EnsureMaster(Plugin& p, const std::string& masterName) {
    for (size_t i = 0; i < p.masters.size(); ++i)
        if (EqualCI(p.masters[i], masterName)) return static_cast<int>(i);

    if (p.masters.size() >= 0xFDu)
        return Fail("EnsureMaster: master limit reached");

    const uint32_t oldSelf = static_cast<uint32_t>(p.masters.size());
    const uint32_t newSelf = oldSelf + 1u;

    // Renumbering needs every FormID each record carries. Decode them all and
    // check remap safety BEFORE mutating anything, so a refusal leaves the
    // plugin untouched.
    std::vector<std::pair<RecordNode*, Record>> decoded;
    decoded.reserve(p.index.size());
    for (auto& [formId, node] : p.index) {
        (void)formId;
        auto rec = Record::Decode(p, *node);
        if (!rec)
            return Wrap(rec.error(), "EnsureMaster: record {:08X} ({}) can't be renumbered",
                        node->header.formId, SigName(node->header.sig));
        if (auto safe = rec->CheckRemapSafe(); !safe)
            return Wrap(safe.error(), "EnsureMaster: record {:08X} ({}) can't be renumbered",
                        node->header.formId, SigName(node->header.sig));
        decoded.emplace_back(node, std::move(*rec));
    }

    p.masters.push_back(masterName);
    p.masterData.push_back(0);

    // Renumber: payload references first, then record headers.
    const SelfIndexBump bump{ oldSelf, newSelf };
    for (auto& [node, rec] : decoded) {
        bool changed = false;
        if (auto visited = rec.VisitFormIds([&](FormId& id) { changed |= bump.Apply(id.raw); }); !visited)
            return Wrap(visited.error(), "EnsureMaster: remap of {:08X} failed", node->header.formId);
        if (changed) {
            if (auto r = SetRecordPayload(p, *node, rec.Encode()); !r) return std::unexpected(r.error());
        }
    }
    for (auto& [formId, node] : p.index) {
        (void)formId;
        uint32_t id = node->header.formId;
        bump.Apply(id);
        node->header.formId = id;
    }
    RebuildIndex(p);
    p.edited = true;
    return static_cast<int>(oldSelf);
}

Result<uint32_t> MakeRef(Plugin& p, const FormKey& key) {
    if (!key.valid())
        return Fail("MakeRef: invalid FormKey");
    const uint32_t localId = key.localId & 0x00FFFFFFu;

    if (EqualCI(key.plugin, p.filename))
        return (static_cast<uint32_t>(p.masters.size()) << 24) | localId;

    const auto idx = EnsureMaster(p, key.plugin);
    if (!idx) return std::unexpected(idx.error());
    return (static_cast<uint32_t>(*idx) << 24) | localId;
}

static GroupNode* FindOrCreateTopGroup(Plugin& p, uint32_t sig) {
    for (Node& n : p.tree)
        if (n.IsGroup() && n.group->header.groupType == group_type::kTop &&
            n.group->header.label == sig)
            return n.group.get();

    Node n;
    n.group = std::make_unique<GroupNode>();
    n.group->header.sig       = Sig4("GRUP");
    n.group->header.groupSize = sizeof(GrupHeader);   // recomputed on save
    n.group->header.label     = sig;
    n.group->header.groupType = group_type::kTop;
    p.tree.push_back(std::move(n));
    return p.tree.back().group.get();
}

Result<RecordNode*> CreateRecord(Plugin& p, uint32_t sig) {
    const auto fid = AllocateFormId(p);
    if (!fid) return std::unexpected(fid.error());
    const uint32_t formId = *fid;

    GroupNode* group = FindOrCreateTopGroup(p, sig);

    Node n;
    n.rec.header         = RecHeader{};
    n.rec.header.sig     = sig;
    n.rec.header.formId  = formId;
    n.rec.header.version = 44;
    n.rec.modified       = std::make_unique<std::vector<uint8_t>>();
    n.rec.dirty          = true;
    group->children.push_back(std::move(n));

    // Appending may have reallocated node storage anywhere in the tree.
    RebuildIndex(p);
    p.edited = true;
    if (RecordNode* node = p.Find(formId)) return node;
    return Fail("CreateRecord: {:08X} not indexed after insert", formId);
}

Result<RecordNode*> InsertRecord(Plugin& p, const RecHeader& header, std::vector<uint8_t> payload) {
    if (p.index.count(header.formId))
        return Fail("InsertRecord: {:08X} already exists in {}", header.formId, p.filename);
    GroupNode* group = FindOrCreateTopGroup(p, header.sig);
    Node n;
    n.rec.header = header;
    group->children.push_back(std::move(n));
    RebuildIndex(p);
    RecordNode* node = p.Find(header.formId);
    if (!node) return Fail("InsertRecord: {:08X} not indexed after insert", header.formId);
    if (auto r = SetRecordPayload(p, *node, std::move(payload)); !r) return std::unexpected(r.error());
    return node;
}

// ── Finalize ────────────────────────────────────────────────────────────────

static size_t CountGroups(const std::vector<Node>& nodes) {
    size_t g = 0;
    for (const Node& n : nodes)
        if (n.IsGroup()) g += 1 + CountGroups(n.group->children);
    return g;
}

Result<void> FinalizePlugin(Plugin& p) {
    // ESL constraint: every SELF-defined record's object id must fit 12 bits.
    if (p.isLight) {
        const uint32_t selfIdx = static_cast<uint32_t>(p.masters.size());
        for (const auto& [formId, node] : p.index) {
            (void)node;
            if ((formId >> 24) == selfIdx && (formId & 0x00FFFFFFu) > 0xFFFu)
                return Fail("FinalizePlugin: ESL record {:08X} exceeds the 12-bit object-id ceiling", formId);
        }
    }

    // HEDR.numRecords = records + groups (locked empirically against vanilla).
    p.hedrNumRecords =
        static_cast<int32_t>(p.index.size() + CountGroups(p.tree));
    if (p.hedrVersion == 0.f) p.hedrVersion = 1.71f;

    // TES4 flags from the decoded booleans.
    uint32_t flags = 0;
    if (p.isMaster)  flags |= rec_flag::kMaster;
    if (p.isLight)   flags |= rec_flag::kLight;
    if (p.localized) flags |= rec_flag::kLocalized;
    p.tes4.header.sig   = sig::TES4;
    p.tes4.header.flags = flags;

    // Rebuild the TES4 payload in canonical subrecord order.
    ByteWriter w;
    {
        ByteWriter hedr;
        hedr.F32(p.hedrVersion);
        hedr.U32(static_cast<uint32_t>(p.hedrNumRecords));
        hedr.U32(p.hedrNextObjectId);
        EmitSubrecord(w, Sig4("HEDR"), hedr.buf.data(), hedr.buf.size());
    }
    if (!p.author.empty()) {
        ByteWriter s;
        s.ZString(p.author);
        EmitSubrecord(w, Sig4("CNAM"), s.buf.data(), s.buf.size());
    }
    if (!p.description.empty()) {
        ByteWriter s;
        s.ZString(p.description);
        EmitSubrecord(w, Sig4("SNAM"), s.buf.data(), s.buf.size());
    }
    for (size_t i = 0; i < p.masters.size(); ++i) {
        ByteWriter m;
        m.ZString(p.masters[i]);
        EmitSubrecord(w, Sig4("MAST"), m.buf.data(), m.buf.size());
        ByteWriter d;
        d.U64(i < p.masterData.size() ? p.masterData[i] : 0ull);
        EmitSubrecord(w, Sig4("DATA"), d.buf.data(), d.buf.size());
    }
    if (!p.onam.empty()) {
        ByteWriter o;
        for (uint32_t id : p.onam) o.U32(id);
        EmitSubrecord(w, Sig4("ONAM"), o.buf.data(), o.buf.size());
    }
    if (p.hasIntv) {
        ByteWriter v;
        v.U32(p.intv);
        EmitSubrecord(w, Sig4("INTV"), v.buf.data(), v.buf.size());
    }
    if (p.hasIncc) {
        ByteWriter v;
        v.U32(p.incc);
        EmitSubrecord(w, Sig4("INCC"), v.buf.data(), v.buf.size());
    }

    p.tes4.modified = std::make_unique<std::vector<uint8_t>>(std::move(w.buf));
    p.tes4.dirty    = true;
    p.edited        = false;
    return {};
}

} // namespace esper
