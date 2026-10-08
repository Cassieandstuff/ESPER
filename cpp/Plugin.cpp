#define _CRT_SECURE_NO_WARNINGS

#include <ESPER/Plugin.h>

#include <ESPER/PluginFlags.h>
#include <ESPER/RecordHeader.h>
#include <ESPER/Subrecords.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace esper {

// ── TES4 decode ─────────────────────────────────────────────────────────────

static void ParseTes4Subrecords(const uint8_t* data, size_t size, Plugin& out) {
    SubIterator it(data, size);
    SubView sub;
    bool pendingMast = false;   // MAST seen, awaiting its paired DATA
    while (it.Next(sub)) {
        if (sub.sig == Sig4("HEDR")) {
            if (sub.size >= 12) {
                std::memcpy(&out.hedrVersion,      sub.data,     4);
                std::memcpy(&out.hedrNumRecords,   sub.data + 4, 4);
                std::memcpy(&out.hedrNextObjectId, sub.data + 8, 4);
            }
        } else if (sub.sig == Sig4("CNAM")) {
            out.author = ReadZString(sub);
        } else if (sub.sig == Sig4("SNAM")) {
            out.description = ReadZString(sub);
        } else if (sub.sig == Sig4("MAST")) {
            if (pendingMast) out.masterData.push_back(0); // MAST without DATA
            out.masters.push_back(ReadZString(sub));
            pendingMast = true;
        } else if (sub.sig == Sig4("DATA")) {
            uint64_t v = 0;
            if (sub.size >= 8) std::memcpy(&v, sub.data, 8);
            if (pendingMast) { out.masterData.push_back(v); pendingMast = false; }
        } else if (sub.sig == Sig4("ONAM")) {
            const uint32_t count = sub.size / 4u;
            out.onam.reserve(out.onam.size() + count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t id = 0;
                std::memcpy(&id, sub.data + i * 4u, 4);
                out.onam.push_back(id);
            }
        } else if (sub.sig == Sig4("INTV")) {
            if (sub.size >= 4) { std::memcpy(&out.intv, sub.data, 4); out.hasIntv = true; }
        } else if (sub.sig == Sig4("INCC")) {
            if (sub.size >= 4) { std::memcpy(&out.incc, sub.data, 4); out.hasIncc = true; }
        }
    }
    if (pendingMast) out.masterData.push_back(0);
}

static void ResetPluginState(Plugin& p) {
    p.isMaster = p.isLight = p.localized = false;
    p.hedrVersion = 0.f;
    p.hedrNumRecords = 0;
    p.hedrNextObjectId = 0;
    p.author.clear();
    p.description.clear();
    p.masters.clear();
    p.masterData.clear();
    p.onam.clear();
    p.hasIntv = false; p.intv = 0;
    p.hasIncc = false; p.incc = 0;
    p.buffer.clear();
    p.tes4 = RecordNode{};
    p.tree.clear();
    p.index.clear();
    p.diagnostics.clear();
}

// ── OpenHeader ──────────────────────────────────────────────────────────────

bool Plugin::OpenHeader(const std::string& path, char* err, int errLen) {
    ResetPluginState(*this);

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::snprintf(err, errLen, "cannot open plugin: %s", path.c_str());
        return false;
    }

    RecHeader hdr{};
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1) {
        std::snprintf(err, errLen, "plugin too small for TES4 header: %s", path.c_str());
        std::fclose(f);
        return false;
    }
    if (hdr.sig != sig::TES4) {
        std::snprintf(err, errLen, "not a TES4 plugin (bad first signature): %s", path.c_str());
        std::fclose(f);
        return false;
    }

    isMaster  = (hdr.flags & rec_flag::kMaster)    != 0;
    isLight   = (hdr.flags & rec_flag::kLight)     != 0;
    localized = (hdr.flags & rec_flag::kLocalized) != 0;

    std::vector<uint8_t> data(hdr.dataSize);
    if (hdr.dataSize > 0 &&
        std::fread(data.data(), 1, hdr.dataSize, f) != hdr.dataSize) {
        std::snprintf(err, errLen, "truncated TES4 data block: %s", path.c_str());
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    ParseTes4Subrecords(data.data(), data.size(), *this);
    return true;
}

// ── FormID resolution (on-disk rule) ────────────────────────────────────────

FormKey Plugin::FormIdToKey(uint32_t formId) const {
    const uint32_t hi      = formId >> 24;
    const uint32_t localId = formId & 0x00FFFFFFu;

    if (hi < masters.size())
        return FormKey{ masters[hi], localId };
    return FormKey{ filename, localId };
}

// ── Group walk ──────────────────────────────────────────────────────────────

namespace {

struct WalkCtx {
    Plugin&        plugin;
    const uint8_t* base;   // buffer start, for diagnostic offsets

    void Diag(const uint8_t* at, const char* fmt, auto... args) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        plugin.diagnostics.push_back(
            Diagnostic{ buf, static_cast<uint64_t>(at - base) });
    }
};

// Parses the [p, end) byte range into `out` children. Malformed entries are
// recorded as diagnostics and skipped past their declared size when that size
// is plausible; only an unrecoverable size (overflowing the parent) ends the
// level early.
void WalkLevel(WalkCtx& ctx, const uint8_t* p, const uint8_t* end,
               std::vector<Node>& out, int depth) {
    if (depth > 64) {
        ctx.Diag(p, "group nesting exceeds depth 64; subtree skipped");
        return;
    }

    while (p + sizeof(RecHeader) <= end) {
        uint32_t sigVal = 0;
        std::memcpy(&sigVal, p, 4);

        if (sigVal == sig::GRUP) {
            GrupHeader gh{};
            std::memcpy(&gh, p, sizeof(gh));
            if (gh.groupSize < sizeof(GrupHeader) ||
                gh.groupSize > static_cast<uint32_t>(end - p)) {
                ctx.Diag(p, "GRUP with invalid groupSize %u (parent has %lld bytes left); level truncated",
                         gh.groupSize, static_cast<long long>(end - p));
                return;
            }

            Node n;
            n.group         = std::make_unique<GroupNode>();
            n.group->header = gh;
            WalkLevel(ctx, p + sizeof(GrupHeader), p + gh.groupSize,
                      n.group->children, depth + 1);
            out.push_back(std::move(n));
            p += gh.groupSize;
        } else {
            RecHeader rh{};
            std::memcpy(&rh, p, sizeof(rh));
            const uint8_t* recData = p + sizeof(RecHeader);
            if (rh.dataSize > static_cast<uint32_t>(end - recData)) {
                char tag[5] = {};
                std::memcpy(tag, &rh.sig, 4);
                ctx.Diag(p, "record %s %08X dataSize %u overflows its group; level truncated",
                         tag, rh.formId, rh.dataSize);
                return;
            }

            Node n;
            n.rec.header  = rh;
            n.rec.raw     = recData;
            n.rec.rawSize = rh.dataSize;
            out.push_back(std::move(n));
            p = recData + rh.dataSize;
        }
    }

    if (p != end)
        ctx.Diag(p, "%lld trailing bytes at end of group (not a full record header)",
                 static_cast<long long>(end - p));
}

// Post-order index build: every RecordNode in the finished tree, keyed by its
// raw on-disk FormID. (Done after the walk so vector reallocation during
// construction cannot invalidate the stored pointers.)
void IndexLevel(WalkCtx& ctx, std::vector<Node>& nodes) {
    for (Node& n : nodes) {
        if (n.IsGroup()) {
            IndexLevel(ctx, n.group->children);
        } else {
            auto [it, inserted] =
                ctx.plugin.index.insert_or_assign(n.rec.header.formId, &n.rec);
            if (!inserted)
                ctx.Diag(n.rec.raw ? n.rec.raw : ctx.base,
                         "duplicate FormID %08X within plugin; later record wins",
                         n.rec.header.formId);
        }
    }
}

} // namespace

// ── Load ────────────────────────────────────────────────────────────────────

bool Plugin::Load(const std::string& path, char* err, int errLen) {
    ResetPluginState(*this);

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::snprintf(err, errLen, "cannot open plugin: %s", path.c_str());
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::snprintf(err, errLen, "cannot seek plugin: %s", path.c_str());
        std::fclose(f);
        return false;
    }
    const long sz = std::ftell(f);
    if (sz < static_cast<long>(sizeof(RecHeader))) {
        std::snprintf(err, errLen, "plugin too small for TES4 header: %s", path.c_str());
        std::fclose(f);
        return false;
    }
    std::rewind(f);

    try {
        buffer.resize(static_cast<size_t>(sz));
    } catch (const std::bad_alloc&) {
        std::snprintf(err, errLen, "out of memory loading plugin (%ld bytes): %s", sz, path.c_str());
        std::fclose(f);
        buffer.clear();
        return false;
    }
    if (std::fread(buffer.data(), 1, buffer.size(), f) != buffer.size()) {
        std::snprintf(err, errLen, "short read on plugin: %s", path.c_str());
        std::fclose(f);
        buffer.clear();
        return false;
    }
    std::fclose(f);

    const uint8_t* base = buffer.data();
    const uint8_t* end  = base + buffer.size();

    RecHeader hdr{};
    std::memcpy(&hdr, base, sizeof(hdr));
    if (hdr.sig != sig::TES4) {
        std::snprintf(err, errLen, "not a TES4 plugin (bad first signature): %s", path.c_str());
        buffer.clear();
        return false;
    }
    const uint8_t* tes4Data = base + sizeof(RecHeader);
    if (hdr.dataSize > static_cast<uint32_t>(end - tes4Data)) {
        std::snprintf(err, errLen, "truncated TES4 data block: %s", path.c_str());
        buffer.clear();
        return false;
    }

    isMaster  = (hdr.flags & rec_flag::kMaster)    != 0;
    isLight   = (hdr.flags & rec_flag::kLight)     != 0;
    localized = (hdr.flags & rec_flag::kLocalized) != 0;

    tes4.header  = hdr;
    tes4.raw     = tes4Data;
    tes4.rawSize = hdr.dataSize;
    ParseTes4Subrecords(tes4Data, hdr.dataSize, *this);

    WalkCtx ctx{ *this, base };
    WalkLevel(ctx, tes4Data + hdr.dataSize, end, tree, 0);
    IndexLevel(ctx, tree);
    return true;
}

} // namespace esper
