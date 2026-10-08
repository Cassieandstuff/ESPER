#define _CRT_SECURE_NO_WARNINGS

#include <ESPER/Writer.h>

#include <ESPER/ByteWriter.h>
#include <ESPER/CaseUtil.h>
#include <ESPER/PluginFlags.h>
#include <ESPER/RecordHeader.h>
#include <ESPER/SchemaRegistry.h>
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

bool SerializePlugin(const Plugin& p, std::vector<uint8_t>& out,
                     char* err, int errLen) {
    if (p.tes4.raw == nullptr && !p.tes4.modified) {
        std::snprintf(err, errLen,
                      "SerializePlugin: plugin '%s' has no TES4 record "
                      "(not Load()ed and not Finalize()d)",
                      p.filename.c_str());
        return false;
    }

    ByteWriter w;
    w.buf.reserve(p.buffer.size() + 4096);

    EmitRecord(w, p.tes4);
    for (const Node& n : p.tree) EmitNode(w, n);

    out = std::move(w.buf);
    return true;
}

bool SavePlugin(const Plugin& p, const std::string& path, char* err, int errLen) {
    std::vector<uint8_t> bytes;
    if (!SerializePlugin(p, bytes, err, errLen)) return false;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::snprintf(err, errLen, "cannot open for write: %s", path.c_str());
        return false;
    }
    const size_t written =
        bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    const bool ok = (written == bytes.size()) && (std::fclose(f) == 0);
    if (!ok)
        std::snprintf(err, errLen, "short write: %s", path.c_str());
    return ok;
}

// ── Mutation ────────────────────────────────────────────────────────────────

void CreateEmptyPlugin(Plugin& p, const std::string& filename, bool light) {
    p = Plugin{};
    p.filename = filename;
    p.isLight  = light;

    p.hedrVersion      = 1.71f;
    p.hedrNumRecords   = 0;
    p.hedrNextObjectId = 0x800u;

    p.tes4.header         = RecHeader{};
    p.tes4.header.sig     = sig::TES4;
    p.tes4.header.version = 44;
    // Payload synthesized by FinalizePlugin.
}

uint32_t AllocateFormId(Plugin& p, char* err, int errLen) {
    if (p.masters.size() > 0xFDu) {
        std::snprintf(err, errLen, "AllocateFormId: %zu masters exceeds the format limit",
                      p.masters.size());
        return 0;
    }
    if (p.hedrNextObjectId < 0x800u) p.hedrNextObjectId = 0x800u;

    const uint32_t objectId = p.hedrNextObjectId;
    if (p.isLight && objectId > 0xFFFu) {
        std::snprintf(err, errLen,
                      "AllocateFormId: ESL object-id space exhausted (next 0x%X > 0xFFF)",
                      objectId);
        return 0;
    }
    if (objectId > 0x00FFFFFFu) {
        std::snprintf(err, errLen, "AllocateFormId: object-id space exhausted");
        return 0;
    }
    ++p.hedrNextObjectId;
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

bool SetRecordPayload(Plugin& p, RecordNode& node,
                      std::vector<uint8_t> payload, char* err, int errLen) {
    (void)p;
    if (node.Compressed()) {
        std::vector<uint8_t> z;
        if (!ZlibDeflate(payload.data(), payload.size(), z, err, errLen))
            return false;
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
    return true;
}

namespace {
struct SelfIndexBump {
    uint32_t oldSelf;
    uint32_t newSelf;
    static void Apply(uint32_t& id, void* ctxRaw) {
        auto* ctx = static_cast<SelfIndexBump*>(ctxRaw);
        // A null FormID (0) is "no reference", not object 0 of master 0 —
        // it must never be renumbered (a masterless plugin has oldSelf == 0,
        // which would otherwise capture every null ref).
        if (id == 0) return;
        if ((id >> 24) == ctx->oldSelf)
            id = (ctx->newSelf << 24) | (id & 0x00FFFFFFu);
    }
};
} // namespace

int EnsureMaster(Plugin& p, const std::string& masterName, char* err, int errLen) {
    for (size_t i = 0; i < p.masters.size(); ++i)
        if (EqualCI(p.masters[i], masterName)) return static_cast<int>(i);

    if (p.masters.size() >= 0xFDu) {
        std::snprintf(err, errLen, "EnsureMaster: master limit reached");
        return -1;
    }

    const uint32_t oldSelf = static_cast<uint32_t>(p.masters.size());
    const uint32_t newSelf = oldSelf + 1u;

    // Renumbering self-index records requires seeing every FormID they carry;
    // DRY-RUN the remap (no-op visitor) so every refusal — missing schema,
    // unknown-passthrough bytes, remap-opaque rows like CTDA — surfaces
    // BEFORE anything is mutated, keeping EnsureMaster atomic on failure.
    SchemaCtx ctx;
    ctx.localized = p.localized;
    for (auto& [formId, node] : p.index) {
        (void)formId;
        char perr[256] = {};
        const std::span<const uint8_t> payload = node->Payload(perr, sizeof(perr));
        if (payload.data() == nullptr) {
            std::snprintf(err, errLen, "EnsureMaster: record %08X unreadable: %s",
                          node->header.formId, perr);
            return -1;
        }
        if (payload.empty()) continue;
        const SchemaOps* ops = FindSchema(node->header.sig);
        if (!ops || !ops->remapIds) {
            char tag[5] = {};
            std::memcpy(tag, &node->header.sig, 4);
            std::snprintf(err, errLen,
                          "EnsureMaster: record %08X (%s) has no FormID-visiting "
                          "schema; add masters before creating such records",
                          node->header.formId, tag);
            return -1;
        }
        std::vector<uint8_t> scratch;
        std::string why;
        if (!ops->remapIds(payload, ctx, [](uint32_t&, void*) {}, nullptr,
                           scratch, why)) {
            std::snprintf(err, errLen, "EnsureMaster: record %08X cannot be "
                          "renumbered: %s", node->header.formId, why.c_str());
            return -1;
        }
    }

    p.masters.push_back(masterName);
    p.masterData.push_back(0);

    // Renumber: payload references first (via schema), then record headers.
    SelfIndexBump bump{ oldSelf, newSelf };
    for (auto& [formId, node] : p.index) {
        (void)formId;
        char perr[256] = {};
        const std::span<const uint8_t> payload = node->Payload(perr, sizeof(perr));
        if (payload.empty()) continue;

        const SchemaOps* ops = FindSchema(node->header.sig);
        std::vector<uint8_t> remapped;
        std::string why;
        if (!ops->remapIds(payload, ctx, &SelfIndexBump::Apply, &bump,
                           remapped, why)) {
            std::snprintf(err, errLen, "EnsureMaster: remap of %08X failed: %s",
                          node->header.formId, why.c_str());
            return -1;   // masters already appended; caller should discard the plugin
        }
        if (remapped.size() != payload.size() ||
            std::memcmp(remapped.data(), payload.data(), payload.size()) != 0) {
            if (!SetRecordPayload(p, *node, std::move(remapped), err, errLen))
                return -1;
        }
    }
    for (auto& [formId, node] : p.index) {
        (void)formId;
        uint32_t id = node->header.formId;
        SelfIndexBump::Apply(id, &bump);
        node->header.formId = id;
    }
    RebuildIndex(p);
    return static_cast<int>(oldSelf);
}

uint32_t MakeRef(Plugin& p, const FormKey& key, char* err, int errLen) {
    if (!key.valid()) {
        std::snprintf(err, errLen, "MakeRef: invalid FormKey");
        return 0;
    }
    const uint32_t localId = key.localId & 0x00FFFFFFu;

    if (EqualCI(key.plugin, p.filename))
        return (static_cast<uint32_t>(p.masters.size()) << 24) | localId;

    const int idx = EnsureMaster(p, key.plugin, err, errLen);
    if (idx < 0) return 0;
    return (static_cast<uint32_t>(idx) << 24) | localId;
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

RecordNode* CreateRecord(Plugin& p, uint32_t sig, char* err, int errLen) {
    const uint32_t formId = AllocateFormId(p, err, errLen);
    if (formId == 0) return nullptr;

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
    auto it = p.index.find(formId);
    return (it != p.index.end()) ? it->second : nullptr;
}

// ── Finalize ────────────────────────────────────────────────────────────────

static size_t CountGroups(const std::vector<Node>& nodes) {
    size_t g = 0;
    for (const Node& n : nodes)
        if (n.IsGroup()) g += 1 + CountGroups(n.group->children);
    return g;
}

bool FinalizePlugin(Plugin& p, char* err, int errLen) {
    // ESL constraint: every SELF-defined record's object id must fit 12 bits.
    if (p.isLight) {
        const uint32_t selfIdx = static_cast<uint32_t>(p.masters.size());
        for (const auto& [formId, node] : p.index) {
            (void)node;
            if ((formId >> 24) == selfIdx && (formId & 0x00FFFFFFu) > 0xFFFu) {
                std::snprintf(err, errLen,
                              "FinalizePlugin: ESL record %08X exceeds the 12-bit "
                              "object-id ceiling",
                              formId);
                return false;
            }
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
    return true;
}

} // namespace esper
