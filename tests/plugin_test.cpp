// ── ESPER self-test ────────────────────────────────────────────────
// Standalone, no framework (same pattern as sct-esm's esm_test.cpp: fixtures
// are byte-built synthetic plugins). Phase-0 coverage:
//   T1 — TES4 decode: HEDR / CNAM / SNAM / MAST+DATA / ONAM / INTV, flags.
//   T2 — group tree: nested typed GRUPs preserved (labels + types), records
//        and groups interleaved, full index over EVERY record.
//   T3 — SubIterator: plain walk, XXXX size-extension folding, truncation ok().
//   T4 — RecordNode::Payload: uncompressed view, lazy zlib inflate, and the
//        empty-record vs decompress-failure distinction.
//   T5 — malformed structure: bad groupSize / overflowing record produce
//        diagnostics and truncate only their own level.
//   T6 — FormIdToKey: master index, self index, out-of-range clamp to self.
//   T7 — LoadOrderIndex: override chains across plugins, ResolveRef signature
//        filter, EnumerateWinning determinism, RuntimeFormIdToKey (FE space).
//   T8 — LoadOrderReader: master hoist; Skyrim.ccc must NOT re-enable a plugin
//        the user disabled in plugins.txt, but still adds unlisted CC content.
// Prints PASS/FAIL and exits nonzero on any failure.

#include <ESPER/CaseUtil.h>
#include <ESPER/FormKey.h>
#include <ESPER/GroupTree.h>
#include <ESPER/LoadOrderIndex.h>
#include <ESPER/LoadOrderReader.h>
#include <ESPER/Plugin.h>
#include <ESPER/PluginFlags.h>
#include <ESPER/RecordHeader.h>
#include <ESPER/RecordsActor.h>
#include <ESPER/RecordsMagic.h>
#include <ESPER/RecordsWorld.h>
#include <ESPER/Subrecords.h>
#include <ESPER/WorldIndex.h>
#include <ESPER/Writer.h>

#include "miniz.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace esper;

static int g_failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  FAIL: %s\n", (msg));                                 \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// ── Byte-emit helpers ─────────────────────────────────────────────────────────

static void PutU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
static void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
static void PutU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
static void PutBytes(std::vector<uint8_t>& b, const std::vector<uint8_t>& src) {
    b.insert(b.end(), src.begin(), src.end());
}
static std::vector<uint8_t> ZStr(const char* s) {
    std::vector<uint8_t> out;
    for (const char* c = s; *c; ++c) out.push_back(static_cast<uint8_t>(*c));
    out.push_back(0);
    return out;
}
static std::vector<uint8_t> Sub(uint32_t sig, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    PutU32(out, sig);
    PutU16(out, static_cast<uint16_t>(payload.size()));
    PutBytes(out, payload);
    return out;
}
static std::vector<uint8_t> Rec(uint32_t sig, uint32_t flags, uint32_t formId,
                                const std::vector<uint8_t>& recData) {
    std::vector<uint8_t> out;
    PutU32(out, sig);
    PutU32(out, static_cast<uint32_t>(recData.size()));
    PutU32(out, flags);
    PutU32(out, formId);
    PutU16(out, 0x1234); // timestamp — nonzero, must round-trip later
    PutU16(out, 0x5678); // vcInfo
    PutU16(out, 44);     // version (SSE form version)
    PutU16(out, 0);
    PutBytes(out, recData);
    return out;
}
static std::vector<uint8_t> Grup(uint32_t label, int32_t groupType,
                                 const std::vector<uint8_t>& inner) {
    std::vector<uint8_t> out;
    PutU32(out, Sig4("GRUP"));
    PutU32(out, 24u + static_cast<uint32_t>(inner.size()));
    PutU32(out, label);
    PutU32(out, static_cast<uint32_t>(groupType));
    PutU16(out, 0);
    PutU16(out, 0);
    PutU32(out, 0);
    PutBytes(out, inner);
    return out;
}
static std::vector<uint8_t> HedrPayload(float version, int32_t numRecords,
                                        uint32_t nextObjectId) {
    std::vector<uint8_t> p;
    uint32_t vbits = 0;
    std::memcpy(&vbits, &version, 4);
    PutU32(p, vbits);
    PutU32(p, static_cast<uint32_t>(numRecords));
    PutU32(p, nextObjectId);
    return p;
}
static std::vector<uint8_t> BuildTes4(uint32_t flags,
                                      const std::vector<const char*>& masters,
                                      const std::vector<uint8_t>& extraSubs = {}) {
    std::vector<uint8_t> data;
    PutBytes(data, Sub(Sig4("HEDR"), HedrPayload(1.71f, 42, 0x0D62u)));
    for (const char* m : masters) {
        PutBytes(data, Sub(Sig4("MAST"), ZStr(m)));
        std::vector<uint8_t> d;
        PutU64(d, 0);
        PutBytes(data, Sub(Sig4("DATA"), d));
    }
    PutBytes(data, extraSubs);
    return Rec(sig::TES4, flags, 0u, data);
}
static bool WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty())
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}
static std::vector<uint8_t> ZlibDeflate(const std::vector<uint8_t>& src) {
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(src.size()));
    std::vector<uint8_t> dst(bound);
    mz_ulong dstLen = bound;
    if (mz_compress(dst.data(), &dstLen, src.data(),
                    static_cast<mz_ulong>(src.size())) != MZ_OK) {
        std::printf("  FATAL: mz_compress failed in fixture\n");
        std::exit(2);
    }
    dst.resize(static_cast<size_t>(dstLen));
    return dst;
}
static std::vector<uint8_t> RecCompressed(uint32_t sig, uint32_t flags,
                                          uint32_t formId,
                                          const std::vector<uint8_t>& recData) {
    std::vector<uint8_t> z = ZlibDeflate(recData);
    std::vector<uint8_t> block;
    PutU32(block, static_cast<uint32_t>(recData.size()));
    PutBytes(block, z);

    std::vector<uint8_t> out;
    PutU32(out, sig);
    PutU32(out, static_cast<uint32_t>(block.size()));
    PutU32(out, flags | rec_flag::kCompressed);
    PutU32(out, formId);
    PutU16(out, 0); PutU16(out, 0); PutU16(out, 44); PutU16(out, 0);
    PutBytes(out, block);
    return out;
}

static fs::path MakeTempDir(const char* tag) {
    std::error_code ec;
    for (int i = 0; i < 10000; ++i) {
        fs::path cand = fs::temp_directory_path(ec) /
            (std::string("bp_test_") + tag + "_" + std::to_string(static_cast<unsigned>(
#ifdef _WIN32
                _getpid()
#else
                getpid()
#endif
            )) + "_" + std::to_string(i));
        if (!fs::exists(cand, ec)) {
            if (fs::create_directories(cand, ec) && !ec) return cand;
        }
    }
    return {};
}

// Counts records/groups in a tree.
static void CountTree(const std::vector<Node>& nodes, int& recs, int& groups) {
    for (const Node& n : nodes) {
        if (n.IsGroup()) { ++groups; CountTree(n.group->children, recs, groups); }
        else ++recs;
    }
}

int main() {
    std::error_code ec;

    // ══ T1 + T2: TES4 decode + typed group tree + full index ════════════════════
    std::printf("ESPER T1/T2: TES4 + group tree\n");
    {
        fs::path tmp = MakeTempDir("tree");
        CHECK(!tmp.empty(), "create temp dir");

        // Extra TES4 subs: CNAM author, SNAM description, ONAM (2 ids), INTV.
        std::vector<uint8_t> extra;
        PutBytes(extra, Sub(Sig4("CNAM"), ZStr("Test Author")));
        PutBytes(extra, Sub(Sig4("SNAM"), ZStr("A description")));
        {
            std::vector<uint8_t> onam;
            PutU32(onam, 0x00112233u);
            PutU32(onam, 0x00445566u);
            PutBytes(extra, Sub(Sig4("ONAM"), onam));
        }
        {
            std::vector<uint8_t> intv;
            PutU32(intv, 7u);
            PutBytes(extra, Sub(Sig4("INTV"), intv));
        }

        // Tree: a WRLD top group holding WRLD + its children group (type 1)
        // containing a CELL + cell-children (type 6) with a REFR — plus a
        // plain KYWD-style top group with two records.
        const uint32_t kWrld = 0x000100u, kCell = 0x000200u, kRefr = 0x000300u;
        const uint32_t kMisc1 = 0x000400u, kMisc2 = 0x000500u;
        const uint32_t MISC = Sig4("MISC"), WRLD = Sig4("WRLD"),
                       CELL = Sig4("CELL"), REFR = Sig4("REFR");

        std::vector<uint8_t> refrRec = Rec(REFR, 0, kRefr, Sub(Sig4("NAME"), { 0,1,2,3 }));
        std::vector<uint8_t> cellChildren = Grup(kCell, group_type::kCellTemporary, refrRec);
        std::vector<uint8_t> cellRec = Rec(CELL, 0, kCell, Sub(Sig4("EDID"), ZStr("TestCell")));
        std::vector<uint8_t> cellPair;
        PutBytes(cellPair, cellRec);
        PutBytes(cellPair, cellChildren);
        std::vector<uint8_t> wrldChildren = Grup(kWrld, group_type::kWorldChildren, cellPair);
        std::vector<uint8_t> wrldRec = Rec(WRLD, 0, kWrld, Sub(Sig4("EDID"), ZStr("TestWorld")));
        std::vector<uint8_t> wrldInner;
        PutBytes(wrldInner, wrldRec);
        PutBytes(wrldInner, wrldChildren);

        std::vector<uint8_t> miscInner;
        PutBytes(miscInner, Rec(MISC, 0, kMisc1, Sub(Sig4("EDID"), ZStr("MiscA"))));
        PutBytes(miscInner, Rec(MISC, 0, kMisc2, Sub(Sig4("EDID"), ZStr("MiscB"))));

        std::vector<uint8_t> esm = BuildTes4(rec_flag::kMaster, { "Skyrim.esm" }, extra);
        PutBytes(esm, Grup(WRLD, group_type::kTop, wrldInner));
        PutBytes(esm, Grup(MISC, group_type::kTop, miscInner));
        CHECK(WriteBytes(tmp / "Tree.esm", esm), "write Tree.esm");

        Plugin p;
        p.filename = "Tree.esm";
        char err[256] = {0};
        CHECK(p.Load((tmp / "Tree.esm").string(), err, sizeof(err)), "Load Tree.esm");
        if (g_failures) std::printf("  err='%s'\n", err);

        // T1: TES4 fields.
        CHECK(p.isMaster, "T1 isMaster");
        CHECK(p.hedrNumRecords == 42, "T1 HEDR numRecords == 42");
        CHECK(p.hedrNextObjectId == 0x0D62u, "T1 HEDR nextObjectId == 0xD62");
        CHECK(p.hedrVersion > 1.70f && p.hedrVersion < 1.72f, "T1 HEDR version ~= 1.71");
        CHECK(p.author == "Test Author", "T1 CNAM author");
        CHECK(p.description == "A description", "T1 SNAM description");
        CHECK(p.masters.size() == 1 && p.masters[0] == "Skyrim.esm", "T1 MAST list");
        CHECK(p.masterData.size() == 1, "T1 MAST/DATA pairing");
        CHECK(p.onam.size() == 2 && p.onam[0] == 0x00112233u && p.onam[1] == 0x00445566u,
              "T1 ONAM preserved");
        CHECK(p.hasIntv && p.intv == 7u, "T1 INTV");

        // T2: tree shape.
        CHECK(p.diagnostics.empty(), "T2 no diagnostics on well-formed file");
        for (const auto& d : p.diagnostics)
            std::printf("  diag @%llu: %s\n",
                        static_cast<unsigned long long>(d.offset), d.message.c_str());
        CHECK(p.tree.size() == 2, "T2 two top-level groups");
        int recs = 0, groups = 0;
        CountTree(p.tree, recs, groups);
        CHECK(recs == 5, "T2 five records in tree");
        CHECK(groups == 4, "T2 four groups in tree (2 top + world children + cell temp)");
        CHECK(p.index.size() == 5, "T2 index covers EVERY record (5)");
        CHECK(p.index.count(kRefr) == 1, "T2 nested REFR is indexed");

        if (p.tree.size() == 2 && p.tree[0].IsGroup()) {
            const GroupNode& wrldTop = *p.tree[0].group;
            CHECK(wrldTop.header.groupType == group_type::kTop &&
                  wrldTop.header.label == WRLD, "T2 top group label/type preserved");
            bool sawChildren = false;
            for (const Node& n : wrldTop.children)
                if (n.IsGroup() &&
                    n.group->header.groupType == group_type::kWorldChildren &&
                    n.group->header.label == kWrld)
                    sawChildren = true;
            CHECK(sawChildren, "T2 world-children group (type 1) preserved with WRLD label");
        }

        // Record header preservation (timestamp/vcInfo/version).
        if (auto it = p.index.find(kMisc1); it != p.index.end()) {
            CHECK(it->second->header.timestamp == 0x1234 &&
                  it->second->header.vcInfo == 0x5678 &&
                  it->second->header.version == 44,
                  "T2 record header reserved fields preserved");
        } else CHECK(false, "T2 MiscA present in index");

        fs::remove_all(tmp, ec);
    }

    // ══ T3: SubIterator (plain, XXXX folding, truncation) ═══════════════════════
    std::printf("ESPER T3: SubIterator\n");
    {
        // Plain walk.
        std::vector<uint8_t> payload;
        PutBytes(payload, Sub(Sig4("EDID"), ZStr("Thing")));
        PutBytes(payload, Sub(Sig4("DATA"), { 1, 2, 3, 4 }));
        SubIterator it(payload.data(), payload.size());
        SubView sub;
        CHECK(it.Next(sub) && sub.sig == Sig4("EDID") && ReadZString(sub) == "Thing",
              "T3 first sub EDID");
        CHECK(it.Next(sub) && sub.sig == Sig4("DATA") && sub.size == 4, "T3 second sub DATA");
        CHECK(!it.Next(sub) && it.ok(), "T3 clean exhaustion, ok()==true");

        // XXXX folding: XXXX(4)=len, then a sub whose 16-bit size is 0 and whose
        // real payload is `len` bytes.
        const uint32_t bigLen = 70000; // > 0xFFFF
        std::vector<uint8_t> big;
        {
            std::vector<uint8_t> xxxx;
            PutU32(xxxx, bigLen);
            PutBytes(big, Sub(Sig4("XXXX"), xxxx));
            PutU32(big, Sig4("LAND"));
            PutU16(big, 0);                       // 16-bit size ignored under XXXX
            big.resize(big.size() + bigLen, 0xAB);
            PutBytes(big, Sub(Sig4("EDID"), ZStr("After")));
        }
        SubIterator bigIt(big.data(), big.size());
        CHECK(bigIt.Next(sub) && sub.sig == Sig4("LAND") && sub.size == bigLen &&
              sub.data[0] == 0xAB && sub.data[bigLen - 1] == 0xAB,
              "T3 XXXX-extended sub folded to real size");
        CHECK(bigIt.Next(sub) && sub.sig == Sig4("EDID") && ReadZString(sub) == "After",
              "T3 sub after XXXX pair still reads");
        CHECK(!bigIt.Next(sub) && bigIt.ok(), "T3 clean exhaustion after XXXX");

        // Truncated payload → ok()==false.
        std::vector<uint8_t> trunc = Sub(Sig4("DATA"), { 1, 2, 3, 4 });
        trunc.pop_back();
        SubIterator tIt(trunc.data(), trunc.size());
        CHECK(!tIt.Next(sub) && !tIt.ok(), "T3 truncated sub → ok()==false");
    }

    // ══ T4: RecordNode::Payload ═════════════════════════════════════════════════
    std::printf("ESPER T4: Payload\n");
    {
        fs::path tmp = MakeTempDir("payload");
        CHECK(!tmp.empty(), "create temp dir");

        std::vector<uint8_t> inner;
        PutBytes(inner, Sub(Sig4("EDID"), ZStr("Plain")));
        std::vector<uint8_t> innerZ;
        PutBytes(innerZ, Sub(Sig4("EDID"), ZStr("Squeezed")));

        const uint32_t MISC = Sig4("MISC");
        std::vector<uint8_t> grpInner;
        PutBytes(grpInner, Rec(MISC, 0, 0x000A00u, inner));
        PutBytes(grpInner, RecCompressed(MISC, 0, 0x000A01u, innerZ));
        PutBytes(grpInner, Rec(MISC, 0, 0x000A02u, {}));  // genuinely empty record

        std::vector<uint8_t> esm = BuildTes4(rec_flag::kMaster, {});
        PutBytes(esm, Grup(MISC, group_type::kTop, grpInner));
        CHECK(WriteBytes(tmp / "Pay.esm", esm), "write Pay.esm");

        Plugin p;
        p.filename = "Pay.esm";
        char err[256] = {0};
        CHECK(p.Load((tmp / "Pay.esm").string(), err, sizeof(err)), "Load Pay.esm");

        auto payloadOf = [&](uint32_t id) -> std::span<const uint8_t> {
            auto it = p.index.find(id);
            if (it == p.index.end()) return {};
            char perr[256] = {0};
            return it->second->Payload(perr, sizeof(perr));
        };

        auto plain = payloadOf(0x000A00u);
        CHECK(plain.data() && plain.size() == inner.size() &&
              std::memcmp(plain.data(), inner.data(), inner.size()) == 0,
              "T4 uncompressed payload is a verbatim view");

        auto z = payloadOf(0x000A01u);
        CHECK(z.data() && z.size() == innerZ.size() &&
              std::memcmp(z.data(), innerZ.data(), innerZ.size()) == 0,
              "T4 compressed payload inflates to original bytes");
        auto z2 = payloadOf(0x000A01u);
        CHECK(z2.data() == z.data(), "T4 second Payload() reuses the inflate cache");

        auto empty = payloadOf(0x000A02u);
        CHECK(empty.data() != nullptr && empty.size() == 0,
              "T4 empty record: non-null pointer, zero size (not an error)");

        // A corrupt compressed record: kCompressed flag but 2-byte payload.
        if (auto it = p.index.find(0x000A01u); it != p.index.end()) {
            RecordNode broken;
            broken.header       = it->second->header;
            broken.header.formId = 0xBAD;
            static const uint8_t junk[2] = { 0x78, 0x9C };
            broken.raw     = junk;
            broken.rawSize = 2;
            char perr[256] = {0};
            auto bad = broken.Payload(perr, sizeof(perr));
            CHECK(bad.data() == nullptr && perr[0] != 0,
                  "T4 corrupt compressed record → null data + error text");
        }

        fs::remove_all(tmp, ec);
    }

    // ══ T5: malformed structure → diagnostics, level-local truncation ═══════════
    std::printf("ESPER T5: diagnostics\n");
    {
        fs::path tmp = MakeTempDir("diag");
        CHECK(!tmp.empty(), "create temp dir");

        const uint32_t MISC = Sig4("MISC");
        // Top group 1: contains a record whose dataSize overflows the group.
        std::vector<uint8_t> badInner;
        {
            std::vector<uint8_t> r;
            PutU32(r, MISC);
            PutU32(r, 0x0FFFFFFFu);   // dataSize far beyond the group
            PutU32(r, 0);
            PutU32(r, 0x000B00u);
            PutU16(r, 0); PutU16(r, 0); PutU16(r, 0); PutU16(r, 0);
            PutBytes(badInner, r);
        }
        // Top group 2: healthy — must still be parsed after group 1's trouble.
        std::vector<uint8_t> goodInner;
        PutBytes(goodInner, Rec(MISC, 0, 0x000B01u, Sub(Sig4("EDID"), ZStr("Fine"))));

        std::vector<uint8_t> esm = BuildTes4(rec_flag::kMaster, {});
        PutBytes(esm, Grup(MISC, group_type::kTop, badInner));
        PutBytes(esm, Grup(MISC, group_type::kTop, goodInner));
        CHECK(WriteBytes(tmp / "Diag.esm", esm), "write Diag.esm");

        Plugin p;
        p.filename = "Diag.esm";
        char err[256] = {0};
        CHECK(p.Load((tmp / "Diag.esm").string(), err, sizeof(err)),
              "T5 Load succeeds despite malformed record");
        CHECK(!p.diagnostics.empty(), "T5 malformed record produced a diagnostic");
        CHECK(p.index.count(0x000B01u) == 1,
              "T5 sibling group after the bad one is still fully parsed");
        CHECK(p.index.count(0x000B00u) == 0, "T5 the overflowing record is not indexed");

        fs::remove_all(tmp, ec);
    }

    // ══ T6 — FormIdToKey handled in esm_test M7(e); quick sanity here too ═══════
    std::printf("ESPER T6: FormIdToKey\n");
    {
        Plugin p;
        p.filename = "Self.esp";
        p.masters  = { "Skyrim.esm", "Update.esm" };
        FormKey a = p.FormIdToKey(0x00013BBAu);
        CHECK(a.plugin == "Skyrim.esm" && a.localId == 0x013BBAu, "T6 master index 0");
        FormKey b = p.FormIdToKey(0x01000800u);
        CHECK(b.plugin == "Update.esm" && b.localId == 0x000800u, "T6 master index 1");
        FormKey c = p.FormIdToKey(0x02000C00u);
        CHECK(c.plugin == "Self.esp" && c.localId == 0x000C00u, "T6 self index");
        FormKey d = p.FormIdToKey(0xFE000800u);
        CHECK(d.plugin == "Self.esp" && d.localId == 0x000800u,
              "T6 on-disk rule: garbage high byte clamps to self (no FE space on disk)");
    }

    // ══ T7: LoadOrderIndex ══════════════════════════════════════════════════════
    std::printf("ESPER T7: LoadOrderIndex\n");
    {
        fs::path tmp = MakeTempDir("index");
        CHECK(!tmp.empty(), "create temp dir");

        const uint32_t MISC = Sig4("MISC"), KYWD = Sig4("KYWD");
        // Master.esm: MISC 0xC00 ("Original") + KYWD 0xC01.
        {
            std::vector<uint8_t> inner;
            PutBytes(inner, Rec(MISC, 0, 0x000C00u, Sub(Sig4("EDID"), ZStr("Original"))));
            std::vector<uint8_t> kInner;
            PutBytes(kInner, Rec(KYWD, 0, 0x000C01u, Sub(Sig4("EDID"), ZStr("SomeKeyword"))));
            std::vector<uint8_t> esm = BuildTes4(rec_flag::kMaster, {});
            PutBytes(esm, Grup(MISC, group_type::kTop, inner));
            PutBytes(esm, Grup(KYWD, group_type::kTop, kInner));
            CHECK(WriteBytes(tmp / "Master.esm", esm), "write Master.esm");
        }
        // Light.esl: kMaster|kLight, one MISC 0x800.
        {
            std::vector<uint8_t> inner;
            PutBytes(inner, Rec(MISC, 0, 0x000800u, Sub(Sig4("EDID"), ZStr("LightThing"))));
            std::vector<uint8_t> esl =
                BuildTes4(rec_flag::kMaster | rec_flag::kLight, {});
            PutBytes(esl, Grup(MISC, group_type::kTop, inner));
            CHECK(WriteBytes(tmp / "Light.esl", esl), "write Light.esl");
        }
        // Override.esp: master = Master.esm; overrides MISC 0xC00 ("Overridden").
        {
            std::vector<uint8_t> inner;
            PutBytes(inner, Rec(MISC, 0, 0x000C00u, Sub(Sig4("EDID"), ZStr("Overridden"))));
            std::vector<uint8_t> esp = BuildTes4(0u, { "Master.esm" });
            PutBytes(esp, Grup(MISC, group_type::kTop, inner));
            CHECK(WriteBytes(tmp / "Override.esp", esp), "write Override.esp");
        }

        std::vector<std::unique_ptr<Plugin>> plugins;
        char err[256] = {0};
        for (const char* name : { "Master.esm", "Light.esl", "Override.esp" }) {
            auto p = std::make_unique<Plugin>();
            p->filename = name;
            CHECK(p->Load((tmp / name).string(), err, sizeof(err)), "T7 load plugin");
            plugins.push_back(std::move(p));
        }
        const Plugin* espPtr = plugins[2].get();

        LoadOrderIndex idx;
        idx.Build(std::move(plugins));

        // Override chain: winner is the ESP's copy.
        const ResolvedRecord* win = idx.Resolve(FormKey{ "Master.esm", 0x000C00u });
        CHECK(win && win->owner && win->owner->filename == "Override.esp",
              "T7 winner comes from the override plugin");
        const auto* chain = idx.ResolveChain(FormKey{ "master.esm", 0x000C00u });
        CHECK(chain && chain->size() == 2, "T7 chain has 2 versions (CI plugin lookup)");

        // ResolveRef with signature filter.
        const ResolvedRecord* byRef = idx.ResolveRef(espPtr, 0x00000C00u, MISC);
        CHECK(byRef != nullptr, "T7 ResolveRef(sig match) resolves");
        CHECK(idx.ResolveRef(espPtr, 0x00000C00u, KYWD) == nullptr,
              "T7 ResolveRef(sig mismatch) returns null");

        // EnumerateWinning: per-sig, deterministic.
        std::vector<const ResolvedRecord*> a, b;
        idx.EnumerateWinning(MISC, a);
        idx.EnumerateWinning(MISC, b);
        CHECK(a.size() == 2, "T7 EnumerateWinning(MISC) finds 2 (Master's + Light's)");
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i) same = (a[i] == b[i]);
        CHECK(same, "T7 EnumerateWinning is stable");
        std::vector<const ResolvedRecord*> k;
        idx.EnumerateWinning(KYWD, k);
        CHECK(k.size() == 1, "T7 EnumerateWinning(KYWD) finds only the KYWD");

        // Runtime FE mapping: regular order [Master.esm, Override.esp],
        // light order [Light.esl] at slot 0.
        CHECK(idx.RegularOrder().size() == 2 && idx.EslOrder().size() == 1,
              "T7 runtime slot split (2 regular, 1 light)");
        FormKey rt = idx.RuntimeFormIdToKey(0xFE000800u);
        CHECK(rt.plugin == "Light.esl" && rt.localId == 0x800u,
              "T7 RuntimeFormIdToKey 0xFE000800 → (Light.esl, 0x800)");
        FormKey rt0 = idx.RuntimeFormIdToKey(0x00000C00u);
        CHECK(rt0.plugin == "Master.esm" && rt0.localId == 0xC00u,
              "T7 RuntimeFormIdToKey regular index 0 → Master.esm");
        CHECK(!idx.RuntimeFormIdToKey(0xFF000001u).valid(),
              "T7 RuntimeFormIdToKey 0xFF (runtime form) → invalid");
        CHECK(!idx.RuntimeFormIdToKey(0xFE00A123u).valid(),
              "T7 RuntimeFormIdToKey out-of-range FE slot → invalid");

        fs::remove_all(tmp, ec);
    }

    // ══ T8: LoadOrderReader — hoist + ccc-vs-plugins.txt precedence ═════════════
    std::printf("ESPER T8: LoadOrderReader\n");
    {
        fs::path tmp = MakeTempDir("order");
        CHECK(!tmp.empty(), "create temp dir");

        CHECK(WriteBytes(tmp / "Late.esm", BuildTes4(rec_flag::kMaster, {})), "write Late.esm");
        CHECK(WriteBytes(tmp / "Early.esp", BuildTes4(0u, {})), "write Early.esp");
        CHECK(WriteBytes(tmp / "ccNew.esl",
                         BuildTes4(rec_flag::kMaster | rec_flag::kLight, {})), "write ccNew.esl");
        CHECK(WriteBytes(tmp / "ccOff.esl",
                         BuildTes4(rec_flag::kMaster | rec_flag::kLight, {})), "write ccOff.esl");
        // ESPFE: light flag on an .esp — must NOT hoist.
        CHECK(WriteBytes(tmp / "Espfe.esp", BuildTes4(rec_flag::kLight, {})), "write Espfe.esp");

        // plugins.txt: ESP first, master after (hoist must fix), ccOff DISABLED.
        {
            std::ofstream pf(tmp / "plugins.txt", std::ios::binary | std::ios::trunc);
            pf << "*Early.esp\n"
               << "*Late.esm\n"
               << "*Espfe.esp\n"
               << "ccOff.esl\n";     // listed but NOT starred = user-disabled
        }
        // Skyrim.ccc names both CC plugins; only the unlisted one may be added.
        {
            std::ofstream cf(tmp / "Skyrim.ccc", std::ios::binary | std::ios::trunc);
            cf << "ccNew.esl\nccOff.esl\n";
        }

        std::vector<PluginListing> order;
        char err[256] = {0};
        CHECK(LoadOrderReader::ResolveOrder(tmp.string(), (tmp / "plugins.txt").string(),
                                            order, err, sizeof(err)),
              "T8 ResolveOrder ok");

        auto find = [&](const char* name) -> const PluginListing* {
            for (const auto& l : order)
                if (EqualCI(l.filename, name)) return &l;
            return nullptr;
        };
        auto indexOf = [&](const char* name) -> int {
            for (size_t i = 0; i < order.size(); ++i)
                if (EqualCI(order[i].filename, name)) return static_cast<int>(i);
            return -1;
        };

        CHECK(order.size() == 5, "T8 five entries resolved");

        const PluginListing* off = find("ccOff.esl");
        CHECK(off && !off->enabled,
              "T8 ccc does NOT re-enable a plugin the user disabled in plugins.txt");
        const PluginListing* nw = find("ccNew.esl");
        CHECK(nw && nw->enabled, "T8 unlisted ccc plugin is added enabled");

        // Hoist: all masters (Late.esm + both .esl) before Early.esp/Espfe.esp.
        const int lateI = indexOf("Late.esm"), earlyI = indexOf("Early.esp"),
                  espfeI = indexOf("Espfe.esp"), newI = indexOf("ccNew.esl");
        CHECK(lateI >= 0 && earlyI >= 0 && lateI < earlyI,
              "T8 Late.esm hoisted above Early.esp");
        CHECK(newI >= 0 && newI < earlyI, "T8 ccNew.esl (master) above the ESPs");
        CHECK(espfeI > lateI && espfeI > newI,
              "T8 ESPFE (light .esp, not master-flagged) does NOT hoist");
        const PluginListing* espfe = find("Espfe.esp");
        CHECK(espfe && espfe->isLight && !espfe->isMaster,
              "T8 ESPFE classified light but not master");

        fs::remove_all(tmp, ec);
    }

    // ══ T9: verbatim writer round-trip ══════════════════════════════════════════
    std::printf("ESPER T9: writer round-trip\n");
    {
        fs::path tmp = MakeTempDir("emit");
        CHECK(!tmp.empty(), "create temp dir");

        // A plugin exercising every structural feature: nested typed groups,
        // a compressed record, and a preserved-reserved-fields record.
        const uint32_t MISC = Sig4("MISC"), CELL = Sig4("CELL"), REFR = Sig4("REFR");
        std::vector<uint8_t> refr = Rec(REFR, 0, 0x000E01u, Sub(Sig4("NAME"), { 9, 9, 9, 9 }));
        std::vector<uint8_t> cellPair;
        PutBytes(cellPair, Rec(CELL, 0, 0x000E00u, Sub(Sig4("EDID"), ZStr("RtCell"))));
        PutBytes(cellPair, Grup(0x000E00u, group_type::kCellTemporary, refr));
        std::vector<uint8_t> miscInner;
        PutBytes(miscInner, Rec(MISC, 0, 0x000E02u, Sub(Sig4("EDID"), ZStr("RtMisc"))));
        PutBytes(miscInner, RecCompressed(MISC, 0, 0x000E03u,
                                          Sub(Sig4("EDID"), ZStr("RtSqueezed"))));

        std::vector<uint8_t> esm = BuildTes4(rec_flag::kMaster, { "Skyrim.esm" });
        PutBytes(esm, Grup(CELL, group_type::kTop, cellPair));
        PutBytes(esm, Grup(MISC, group_type::kTop, miscInner));
        CHECK(WriteBytes(tmp / "Rt.esm", esm), "write Rt.esm");

        Plugin p;
        p.filename = "Rt.esm";
        char err[256] = {0};
        CHECK(p.Load((tmp / "Rt.esm").string(), err, sizeof(err)), "T9 Load Rt.esm");

        std::vector<uint8_t> emitted;
        CHECK(SerializePlugin(p, emitted, err, sizeof(err)), "T9 SerializePlugin ok");
        CHECK(emitted.size() == esm.size() &&
              std::memcmp(emitted.data(), esm.data(), esm.size()) == 0,
              "T9 unmodified plugin re-emits byte-identically");

        // Reading a compressed record's Payload (populating the inflate cache)
        // must NOT change what gets emitted — raw bytes still carry through.
        if (auto it = p.index.find(0x000E03u); it != p.index.end()) {
            char perr[256] = {0};
            (void)it->second->Payload(perr, sizeof(perr));
        }
        std::vector<uint8_t> emitted2;
        CHECK(SerializePlugin(p, emitted2, err, sizeof(err)), "T9 second serialize ok");
        CHECK(emitted2 == emitted, "T9 inflate cache does not perturb emission");

        // A modified payload replaces the record's bytes and resizes its group.
        if (auto it = p.index.find(0x000E02u); it != p.index.end()) {
            std::vector<uint8_t> repl = Sub(Sig4("EDID"), ZStr("RtMiscRenamedLonger"));
            it->second->modified = std::make_unique<std::vector<uint8_t>>(repl);
            it->second->dirty    = true;
        }
        std::vector<uint8_t> emitted3;
        CHECK(SerializePlugin(p, emitted3, err, sizeof(err)), "T9 modified serialize ok");
        CHECK(emitted3.size() > emitted.size(), "T9 modified emission grew");

        // The modified plugin must itself reload cleanly and round-trip.
        CHECK(WriteBytes(tmp / "Rt2.esm", emitted3), "T9 write modified plugin");
        Plugin p2;
        p2.filename = "Rt2.esm";
        CHECK(p2.Load((tmp / "Rt2.esm").string(), err, sizeof(err)), "T9 reload modified");
        CHECK(p2.diagnostics.empty(), "T9 modified plugin has clean structure");
        CHECK(p2.index.size() == p.index.size(), "T9 modified plugin keeps all records");
        std::vector<uint8_t> emitted4;
        CHECK(SerializePlugin(p2, emitted4, err, sizeof(err)) && emitted4 == emitted3,
              "T9 modified plugin round-trips byte-identically");

        fs::remove_all(tmp, ec);
    }

    // ══ T10: authoring pipeline — create plugin, typed NPC, save, reload ════════
    std::printf("ESPER T10: authoring pipeline\n");
    {
        fs::path tmp = MakeTempDir("author");
        CHECK(!tmp.empty(), "create temp dir");

        // A master defining a RACE the new plugin will reference.
        const uint32_t kRaceId = 0x000A00u;
        {
            std::vector<uint8_t> raceData;
            PutBytes(raceData, Sub(Sig4("EDID"), ZStr("AuthorRace")));
            std::vector<uint8_t> esm = BuildTes4(rec_flag::kMaster, {});
            PutBytes(esm, Grup(Sig4("RACE"), group_type::kTop,
                               Rec(Sig4("RACE"), 0, kRaceId, raceData)));
            CHECK(WriteBytes(tmp / "AuthorMaster.esm", esm), "write AuthorMaster.esm");
        }

        // Fresh plugin → NPC referencing the master's race.
        Plugin np;
        CreateEmptyPlugin(np, "Made_SCT.esp", /*light=*/false);
        char err[256] = {0};

        const uint32_t raceRef =
            MakeRef(np, FormKey{ "AuthorMaster.esm", kRaceId }, err, sizeof(err));
        CHECK(raceRef == kRaceId, "T10 MakeRef → master index 0 ref");
        CHECK(np.masters.size() == 1 && np.masters[0] == "AuthorMaster.esm",
              "T10 MakeRef auto-added the master");

        RecordNode* node = CreateRecord(np, Sig4("NPC_"), err, sizeof(err));
        CHECK(node != nullptr, "T10 CreateRecord NPC_");
        CHECK(node && (node->header.formId >> 24) == 1u &&
              (node->header.formId & 0xFFFFFFu) == 0x800u,
              "T10 new record formId = self-index 1, object 0x800");

        if (node) {
            NpcRec npc;
            CHECK(DecodeNode(kNpcSchema, np, *node, npc, err, sizeof(err)),
                  "T10 DecodeNode on empty new record");
            npc.editorId = "MadeNord";
            npc.fullName = Lstring{};
            npc.fullName->text = "Made Nord";
            npc.race     = FormRef{ raceRef };
            AcbsData acbs{};
            acbs.flags = 0x1u;   // female
            npc.acbs   = acbs;
            CHECK(CommitNode(kNpcSchema, np, *node, npc, err, sizeof(err)),
                  "T10 CommitNode");
        }

        // Adding ANOTHER master after the record exists must renumber the
        // record's self index (1 → 2) and any self refs in its payload.
        const int m2 = EnsureMaster(np, "Second.esm", err, sizeof(err));
        CHECK(m2 == 1, "T10 EnsureMaster('Second.esm') at index 1");
        if (m2 < 0) std::printf("  err='%s'\n", err);
        RecordNode* renumbered = nullptr;
        for (auto& [fid, rn] : np.index)
            if (rn->header.sig == Sig4("NPC_")) { renumbered = rn; break; }
        CHECK(renumbered && (renumbered->header.formId >> 24) == 2u,
              "T10 record header self index bumped to 2 after master append");
        // The race ref (master index 0) must be untouched.
        if (renumbered) {
            NpcRec check;
            CHECK(DecodeNode(kNpcSchema, np, *renumbered, check, err, sizeof(err)) &&
                  check.race && check.race->id == kRaceId,
                  "T10 master-index ref survived the renumber");
        }

        CHECK(FinalizePlugin(np, err, sizeof(err)), "T10 FinalizePlugin");
        CHECK(np.hedrNumRecords == 2, "T10 HEDR.numRecords == records(1)+groups(1)");
        const fs::path outPath = tmp / "Made_SCT.esp";
        CHECK(SavePlugin(np, outPath.string(), err, sizeof(err)), "T10 SavePlugin");

        // Reload from disk and verify everything round-tripped.
        Plugin re;
        re.filename = "Made_SCT.esp";
        CHECK(re.Load(outPath.string(), err, sizeof(err)), "T10 reload saved plugin");
        CHECK(re.diagnostics.empty(), "T10 saved plugin has clean structure");
        CHECK(re.masters.size() == 2 && re.masters[0] == "AuthorMaster.esm" &&
              re.masters[1] == "Second.esm", "T10 MAST list round-tripped");
        CHECK(re.hedrNextObjectId == 0x801u, "T10 nextObjectId round-tripped");
        CHECK(re.index.size() == 1, "T10 one record");

        if (!re.index.empty()) {
            RecordNode* rn = re.index.begin()->second;
            NpcRec npc;
            CHECK(DecodeNode(kNpcSchema, re, *rn, npc, err, sizeof(err)),
                  "T10 decode reloaded NPC");
            CHECK(npc.editorId && *npc.editorId == "MadeNord", "T10 EDID round-tripped");
            CHECK(npc.fullName && npc.fullName->text &&
                  *npc.fullName->text == "Made Nord", "T10 FULL round-tripped");
            CHECK(npc.race && npc.race->id == kRaceId, "T10 RNAM round-tripped");
            CHECK(npc.IsFemale(), "T10 ACBS female flag round-tripped");
            CHECK(npc.unknown.empty(), "T10 authored record has zero unknown subs");

            // Resolve through a load order: the ref reaches the master's RACE.
            FormKey raceKey = re.FormIdToKey(npc.race->id);
            CHECK(raceKey.plugin == "AuthorMaster.esm" && raceKey.localId == kRaceId,
                  "T10 reloaded ref resolves to the master's RACE");
        }

        // ESL object-id exhaustion errors cleanly.
        Plugin esl;
        CreateEmptyPlugin(esl, "Tiny.esl", /*light=*/true);
        esl.hedrNextObjectId = 0xFFFu;
        char e1[256] = {0};
        CHECK(AllocateFormId(esl, e1, sizeof(e1)) != 0, "T10 ESL id 0xFFF allocates");
        char e2[256] = {0};
        CHECK(AllocateFormId(esl, e2, sizeof(e2)) == 0 && e2[0] != 0,
              "T10 ESL id past 0xFFF errors cleanly");

        fs::remove_all(tmp, ec);
    }

    // ══ T11: WorldIndex — grid lookup, children merge across override ═══════════
    std::printf("ESPER T11: WorldIndex\n");
    {
        fs::path tmp = MakeTempDir("world");
        CHECK(!tmp.empty(), "create temp dir");

        const uint32_t WRLD = Sig4("WRLD"), CELL = Sig4("CELL"),
                       REFR = Sig4("REFR"), LAND = Sig4("LAND");
        const uint32_t kWrld = 0x000100u, kExtCell = 0x000200u,
                       kRefr = 0x000300u, kLand = 0x000350u,
                       kIntCell = 0x000400u, kIntRefr = 0x000450u;

        auto xclc = [&](int32_t x, int32_t y) {
            std::vector<uint8_t> v;
            PutU32(v, static_cast<uint32_t>(x));
            PutU32(v, static_cast<uint32_t>(y));
            PutU32(v, 0);
            return v;
        };
        auto placement = [&](float px) {
            std::vector<uint8_t> v(24, 0);
            std::memcpy(v.data(), &px, 4);
            return v;
        };

        // Base.esm: WRLD + exterior cell (3,-2) with REFR + LAND, and an
        // interior cell with a REFR.
        {
            std::vector<uint8_t> extCellData;
            PutBytes(extCellData, Sub(Sig4("EDID"), ZStr("ExtCell")));
            PutBytes(extCellData, Sub(Sig4("XCLC"), xclc(3, -2)));

            std::vector<uint8_t> refr =
                Rec(REFR, 0, kRefr, Sub(Sig4("NAME"), { 1, 2, 3, 4 }));
            std::vector<uint8_t> landRec = Rec(LAND, 0, kLand, Sub(Sig4("DATA"), { 0,0,0,0 }));
            std::vector<uint8_t> tempInner;
            PutBytes(tempInner, refr);
            PutBytes(tempInner, landRec);
            std::vector<uint8_t> cellKids =
                Grup(kExtCell, group_type::kCellChildren,
                     Grup(kExtCell, group_type::kCellTemporary, tempInner));

            std::vector<uint8_t> wrldKidsInner;
            PutBytes(wrldKidsInner, Rec(CELL, 0, kExtCell, extCellData));
            PutBytes(wrldKidsInner, cellKids);
            std::vector<uint8_t> wrldInner;
            PutBytes(wrldInner, Rec(WRLD, 0, kWrld, Sub(Sig4("EDID"), ZStr("TWorld"))));
            PutBytes(wrldInner, Grup(kWrld, group_type::kWorldChildren, wrldKidsInner));

            std::vector<uint8_t> intCellData;
            PutBytes(intCellData, Sub(Sig4("EDID"), ZStr("IntCell")));
            std::vector<uint8_t> intKids =
                Grup(kIntCell, group_type::kCellChildren,
                     Grup(kIntCell, group_type::kCellPersistent,
                          Rec(REFR, 0, kIntRefr,
                              Sub(Sig4("DATA"), placement(1.f)))));
            std::vector<uint8_t> intInner;
            PutBytes(intInner, Rec(CELL, 0, kIntCell, intCellData));
            PutBytes(intInner, intKids);

            std::vector<uint8_t> esm = BuildTes4(rec_flag::kMaster, {});
            PutBytes(esm, Grup(WRLD, group_type::kTop, wrldInner));
            PutBytes(esm, Grup(CELL, group_type::kTop,
                               Grup(0, group_type::kInteriorCellBlock,
                                    Grup(0, group_type::kInteriorCellSub, intInner))));
            CHECK(WriteBytes(tmp / "WBase.esm", esm), "write WBase.esm");
        }

        // Over.esp: overrides the exterior CELL RECORD only (no children) —
        // the base game's refs and LAND must survive the merge.
        {
            std::vector<uint8_t> cellData;
            PutBytes(cellData, Sub(Sig4("EDID"), ZStr("ExtCellRenamed")));
            PutBytes(cellData, Sub(Sig4("XCLC"), xclc(3, -2)));
            std::vector<uint8_t> wrldKidsInner;
            PutBytes(wrldKidsInner, Rec(CELL, 0, kExtCell, cellData));
            std::vector<uint8_t> wrldInner =
                Grup(kWrld, group_type::kWorldChildren, wrldKidsInner);

            std::vector<uint8_t> esp = BuildTes4(0u, { "WBase.esm" });
            PutBytes(esp, Grup(WRLD, group_type::kTop, wrldInner));
            CHECK(WriteBytes(tmp / "WOver.esp", esp), "write WOver.esp");
        }

        std::vector<std::unique_ptr<Plugin>> plugins;
        char err[256] = {0};
        for (const char* name : { "WBase.esm", "WOver.esp" }) {
            auto p = std::make_unique<Plugin>();
            p->filename = name;
            CHECK(p->Load((tmp / name).string(), err, sizeof(err)), "T11 load plugin");
            plugins.push_back(std::move(p));
        }
        LoadOrderIndex idx;
        idx.Build(std::move(plugins));
        WorldIndex world;
        world.Build(idx);

        CHECK(world.ExteriorCellCount() == 1 && world.InteriorCellCount() == 1,
              "T11 one exterior + one interior");

        const FormKey worldKey{ "WBase.esm", kWrld };
        const CellEntry* ext = world.FindExterior(idx, worldKey, 3, -2);
        CHECK(ext != nullptr, "T11 FindExterior (3,-2)");
        if (ext) {
            CHECK(ext->owner && ext->owner->filename == "WOver.esp",
                  "T11 winning CELL record comes from the override");
            CHECK(ext->childOwner && ext->childOwner->filename == "WBase.esm",
                  "T11 children stayed with the base (record-only override)");
            CHECK(ext->land != nullptr, "T11 LAND survived the merge");
            std::vector<PlacedRef> refs;
            WorldIndex::CollectPlacedRefs(*ext, refs);
            CHECK(refs.size() == 1 && refs[0].rec->header.formId == kRefr,
                  "T11 base REFR survived, LAND/NAVM excluded from refs");
        }
        CHECK(world.FindExterior(idx, worldKey, 9, 9) == nullptr,
              "T11 absent grid coords miss");

        const CellEntry* interior =
            world.FindInterior(idx, FormKey{ "WBase.esm", kIntCell });
        CHECK(interior != nullptr, "T11 FindInterior");
        if (interior) {
            std::vector<PlacedRef> refs;
            WorldIndex::CollectPlacedRefs(*interior, refs);
            CHECK(refs.size() == 1 && refs[0].rec->header.formId == kIntRefr,
                  "T11 interior persistent REFR collected");
        }

        fs::remove_all(tmp, ec);
    }

    // ══ T12: condition remap safety + null-FormID renumber guard ════════════════
    std::printf("ESPER T12: remap safety\n");
    {
        fs::path tmp = MakeTempDir("remap");
        CHECK(!tmp.empty(), "create temp dir");

        char err[512] = {0};

        // (a) A condition using an UNKNOWN function refuses the remap — the
        //     safety net: we can't classify its params, so we won't touch them.
        {
            Plugin p;
            CreateEmptyPlugin(p, "UnkCond.esp", /*light=*/false);
            RecordNode* sn = CreateRecord(p, Sig4("SPEL"), err, sizeof(err));
            CHECK(sn != nullptr, "T12 create SPEL (unknown fn)");
            if (sn) {
                SpellRec spell;
                DecodeNode(kSpelSchema, p, *sn, spell, err, sizeof(err));
                spell.editorId = "UnkCond";
                CondData c{};
                c.functionIndex = 60000;         // not in the table → unknown
                c.param1        = 0x00000800u;   // maybe a form — but we don't know
                spell.conds.conditions.push_back(c);
                CHECK(CommitNode(kSpelSchema, p, *sn, spell, err, sizeof(err)),
                      "T12 commit unknown-fn SPEL");
            }
            char rerr[512] = {0};
            const int idx = EnsureMaster(p, "Skyrim.esm", rerr, sizeof(rerr));
            CHECK(idx < 0, "T12 EnsureMaster refuses over an UNKNOWN condition function");
            CHECK(std::strstr(rerr, "CTDA") != nullptr, "T12 refusal message names CTDA");
            CHECK(p.masters.empty(), "T12 refused EnsureMaster is atomic");
            if (idx >= 0 || !std::strstr(rerr, "CTDA")) std::printf("  err='%s'\n", rerr);
        }

        // (b) KNOWN condition functions remap correctly: GetInFaction's Form
        //     param (a self-ref) is renumbered on master insert; GetIsSex's Int
        //     param — which happens to look like a self-ref — is left untouched.
        {
            Plugin p;
            CreateEmptyPlugin(p, "KnownCond.esp", /*light=*/false);
            RecordNode* sn = CreateRecord(p, Sig4("SPEL"), err, sizeof(err));
            CHECK(sn != nullptr, "T12 create SPEL (known fn)");
            if (sn) {
                SpellRec spell;
                DecodeNode(kSpelSchema, p, *sn, spell, err, sizeof(err));
                spell.editorId = "KnownCond";
                CondData formCond{};
                formCond.functionIndex = 71;          // GetInFaction — p1 = Form
                formCond.param1        = 0x00000801u; // self-index ref (top byte 0)
                spell.conds.conditions.push_back(formCond);
                CondData intCond{};
                intCond.functionIndex = 70;           // GetIsSex — p1 = Int
                intCond.param1        = 0x00000123u;  // int that resembles a self-ref
                spell.conds.conditions.push_back(intCond);
                CHECK(CommitNode(kSpelSchema, p, *sn, spell, err, sizeof(err)),
                      "T12 commit known-fn SPEL");
            }
            const int idx = EnsureMaster(p, "Skyrim.esm", err, sizeof(err));
            CHECK(idx == 0, "T12 EnsureMaster succeeds over KNOWN condition functions");
            if (idx < 0) std::printf("  err='%s'\n", err);

            RecordNode* rn = nullptr;
            for (auto& [fid, n] : p.index) if (n->header.sig == Sig4("SPEL")) rn = n;
            if (rn) {
                SpellRec spell;
                CHECK(DecodeNode(kSpelSchema, p, *rn, spell, err, sizeof(err)) &&
                      spell.conds.conditions.size() == 2, "T12 re-decode known-fn SPEL");
                if (spell.conds.conditions.size() == 2) {
                    CHECK((spell.conds.conditions[0].param1 >> 24) == 1u &&
                          (spell.conds.conditions[0].param1 & 0xFFFFFFu) == 0x801u,
                          "T12 GetInFaction Form param self-index bumped 0 → 1");
                    CHECK(spell.conds.conditions[1].param1 == 0x00000123u,
                          "T12 GetIsSex Int param left untouched by the remap");
                }
            }
        }

        // (c) Null-ref renumber guard: the same renumber must succeed and the
        // null reference must stay null (the oldSelf==0 capture bug).
        Plugin p2;
        CreateEmptyPlugin(p2, "Safety2.esp", /*light=*/false);
        RecordNode* n2 = CreateRecord(p2, Sig4("NPC_"), err, sizeof(err));
        CHECK(n2 != nullptr, "T12 create NPC in second plugin");
        if (n2) {
            NpcRec npc;
            CHECK(DecodeNode(kNpcSchema, p2, *n2, npc, err, sizeof(err)),
                  "T12 decode");
            npc.editorId  = "NullRefNord";
            npc.deathItem = FormRef{ 0 };
            CHECK(CommitNode(kNpcSchema, p2, *n2, npc, err, sizeof(err)),
                  "T12 commit");
        }
        const int m = EnsureMaster(p2, "Skyrim.esm", err, sizeof(err));
        CHECK(m == 0, "T12 EnsureMaster succeeds without conditions");
        if (m < 0) std::printf("  err='%s'\n", err);
        RecordNode* after = nullptr;
        for (auto& [fid, rn] : p2.index)
            if (rn->header.sig == Sig4("NPC_")) after = rn;
        CHECK(after && (after->header.formId >> 24) == 1u,
              "T12 record header renumbered 0 → 1");
        if (after) {
            NpcRec npc;
            CHECK(DecodeNode(kNpcSchema, p2, *after, npc, err, sizeof(err)) &&
                  npc.deathItem && npc.deathItem->id == 0u,
                  "T12 null FormID stayed null through the renumber");
        }

        fs::remove_all(tmp, ec);
    }

    if (g_failures == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL (%d check(s) failed)\n", g_failures);
    return 1;
}
