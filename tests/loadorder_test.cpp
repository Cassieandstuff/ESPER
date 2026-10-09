// ── esper::loadorder self-test ──────────────────────────────────────────────
// Builds a small load order on disk with ESPER's own writer — a localized
// master with a string table, and a mod that overrides it — then drives
// LoadOrderDocument through resolution, search, typed decoding, link
// following, the snapshot, and OverrideInto (masters, FormID rewriting,
// relocalization, refusal, save + reload).

#include <ESPER/LoadOrder.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace f = esper::fields;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                         \
    } while (0)

constexpr uint32_t kArmo = esper::Sig4("ARMO");
constexpr uint32_t kKywd = esper::Sig4("KYWD");

void Put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// A .STRINGS file: count, data size, directory (id, offset), NUL-terminated data.
std::vector<uint8_t> StringsFile(const std::vector<std::pair<uint32_t, std::string>>& entries) {
    std::vector<uint8_t> data, out;
    std::vector<std::pair<uint32_t, uint32_t>> dir;
    for (const auto& [id, text] : entries) {
        dir.emplace_back(id, static_cast<uint32_t>(data.size()));
        data.insert(data.end(), text.begin(), text.end());
        data.push_back(0);
    }
    Put32(out, static_cast<uint32_t>(entries.size()));
    Put32(out, static_cast<uint32_t>(data.size()));
    for (const auto& [id, off] : dir) { Put32(out, id); Put32(out, off); }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

void WriteFile(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                            static_cast<std::streamsize>(bytes.size()));
}

// A new record of `sig` in `p`, filled by `fill` through the schema engine.
template <class Fill>
uint32_t AddRecord(esper::Plugin& p, uint32_t sig, Fill&& fill) {
    char err[256] = {};
    esper::RecordNode* node = esper::CreateRecord(p, sig, err, sizeof(err));
    if (!node) { std::printf("  CreateRecord: %s\n", err); return 0; }
    const uint32_t id = node->header.formId;
    esper::Record rec;
    esper::DecodeCtx ctx;
    ctx.localized = p.localized;
    esper::Record::Create(esper::schema::Main(), sig, ctx, rec);
    fill(rec);
    std::string why;
    if (!esper::Record::Commit(p, *p.index.at(id), rec, &why)) std::printf("  Commit: %s\n", why.c_str());
    return id;
}

void Save(esper::Plugin& p, const fs::path& path) {
    char err[256] = {};
    if (!esper::FinalizePlugin(p, err, sizeof(err)) || !esper::SavePlugin(p, path.string(), err, sizeof(err)))
        std::printf("  save %s: %s\n", path.string().c_str(), err);
}

struct Fixture {
    fs::path dir;
    uint32_t kwA = 0, armorA = 0, armorLoc = 0, armorRaw = 0;   // in Master.esm
    uint32_t kwB = 0;                                            // in Mod.esp
};

Fixture Build() {
    Fixture fx;
    fx.dir = fs::temp_directory_path() / "esper_loadorder_test";
    fs::remove_all(fx.dir);
    fs::create_directories(fx.dir / "Data");

    // Master.esm — localized; FULL values are string ids.
    esper::Plugin master;
    esper::CreateEmptyPlugin(master, "Master.esm", false);
    master.isMaster = true;
    master.localized = true;
    fx.kwA = AddRecord(master, kKywd, [](esper::Record& r) { r.Set(f::KYWD::editorId, std::string("KwA")); });
    fx.armorA = AddRecord(master, kArmo, [&](esper::Record& r) {
        r.Set(f::ARMO::editorId, std::string("ArmorA"));
        r.Set(f::ARMO::fullName, esper::LocalizedString{ 1, {} });
        r.Set(f::ARMO::keywords, std::vector<esper::FormId>{ { fx.kwA } });
    });
    fx.armorLoc = AddRecord(master, kArmo, [&](esper::Record& r) {
        r.Set(f::ARMO::editorId, std::string("ArmorLoc"));
        r.Set(f::ARMO::fullName, esper::LocalizedString{ 2, {} });
        r.Set(f::ARMO::keywords, std::vector<esper::FormId>{ { fx.kwA } });
    });
    // A record with a subrecord the schema doesn't know: FormIDs not all visible.
    fx.armorRaw = AddRecord(master, kArmo, [](esper::Record& r) { r.Set(f::ARMO::editorId, std::string("ArmorRaw")); });
    {
        esper::ByteWriter w;
        const char edid[] = "ArmorRaw";
        esper::EmitSubrecord(w, esper::Sig4("EDID"), edid, sizeof(edid));
        const uint8_t junk[4] = { 1, 2, 3, 4 };
        esper::EmitSubrecord(w, esper::Sig4("ZZZZ"), junk, 4);
        char err[128] = {};
        esper::SetRecordPayload(master, *master.index.at(fx.armorRaw), std::move(w.buf), err, sizeof(err));
    }
    Save(master, fx.dir / "Data" / "Master.esm");
    WriteFile(fx.dir / "Data" / "Strings" / "Master_English.STRINGS",
              StringsFile({ { 1, "Armor A (localized)" }, { 2, "Localized Armor" } }));

    // Mod.esp — overrides ArmorA (new inline name, an extra keyword) and adds KwB.
    esper::Plugin mod;
    esper::CreateEmptyPlugin(mod, "Mod.esp", false);
    char err[256] = {};
    const uint32_t kwAInMod = esper::MakeRef(mod, esper::FormKey{ "Master.esm", fx.kwA & 0xFFFFFF }, err, sizeof(err));
    const uint32_t armorAInMod = esper::MakeRef(mod, esper::FormKey{ "Master.esm", fx.armorA & 0xFFFFFF }, err, sizeof(err));
    fx.kwB = AddRecord(mod, kKywd, [](esper::Record& r) { r.Set(f::KYWD::editorId, std::string("KwB")); });
    {
        esper::Record rec;
        esper::Record::Create(esper::schema::Main(), kArmo, {}, rec);
        rec.Set(f::ARMO::editorId, std::string("ArmorA"));
        rec.Set(f::ARMO::fullName, esper::LocalizedString{ 0, "Armor A (modded)" });
        rec.Set(f::ARMO::keywords, std::vector<esper::FormId>{ { kwAInMod }, { fx.kwB } });
        esper::RecHeader h{};
        h.sig = kArmo;
        h.formId = armorAInMod;
        h.version = 44;
        esper::InsertRecord(mod, h, rec.Encode(), err, sizeof(err));
    }
    Save(mod, fx.dir / "Data" / "Mod.esp");

    std::ofstream(fx.dir / "plugins.txt") << "*Master.esm\n*Mod.esp\n";
    return fx;
}

esper::FormKey Key(const char* plugin, uint32_t id) { return { plugin, id & 0xFFFFFF }; }

} // namespace

int main() {
    const Fixture fx = Build();
    esper::LoadOrderDocument doc;
    esper::LoadOrderOptions opt;
    opt.dataFolder = (fx.dir / "Data").string();
    opt.pluginsTxt = (fx.dir / "plugins.txt").string();
    std::string err;
    CHECK(doc.Load(opt, &err));
    if (!doc.IsLoaded()) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("L1 load + resolution\n");
    CHECK(doc.Plugins().size() == 2);
    CHECK(doc.Warnings().empty());
    const esper::FormKey armorA = Key("Master.esm", fx.armorA);
    const esper::ResolvedRecord* winner = doc.Resolve(armorA);
    const esper::ResolvedRecord* origin = doc.Resolve(armorA, esper::ResolveTarget::Origin);
    CHECK(winner && winner->owner->filename == "Mod.esp");
    CHECK(origin && origin->owner->filename == "Master.esm");
    CHECK(doc.Chain(armorA) && doc.Chain(armorA)->size() == 2);
    CHECK(doc.Enumerate(kKywd).size() == 2);
    CHECK(doc.Enumerate(kArmo).size() == 3);

    std::printf("L2 names + search\n");
    CHECK(winner && doc.Name(*winner) == "Armor A (modded)");
    CHECK(origin && doc.Name(*origin) == "Armor A (localized)");     // via Master_English.STRINGS
    CHECK(winner && doc.EditorId(*winner) == "ArmorA");
    CHECK(doc.Search(kArmo, "localized").size() == 1);                 // ArmorLoc's name
    CHECK(doc.Search(kArmo, "ARMORRAW").size() == 1);                  // EditorID, case-insensitive
    CHECK(doc.Search(kArmo, "").size() == 3);

    std::printf("L3 typed decode + link following\n");
    if (winner) {
        auto d = doc.Decode(*winner);
        CHECK(d.has_value());
        if (d) {
            esper::view::Armor armor(d->record);
            const auto kws = armor.Keywords();
            CHECK(kws && kws->size() == 2);
            if (kws && kws->size() == 2) {
                const esper::ResolvedRecord* a = doc.Follow(*winner, (*kws)[0]);
                const esper::ResolvedRecord* b = doc.Follow(*winner, (*kws)[1]);
                CHECK(a && doc.EditorId(*a) == "KwA");
                CHECK(b && doc.EditorId(*b) == "KwB");
                CHECK(doc.Follow(*winner, esper::Link<esper::view::Armor>{ (*kws)[0].id }) == nullptr);
            }
        }
    }

    std::printf("L4 snapshot + world index\n");
    const std::string snap = doc.SnapshotJson();
    CHECK(snap.find("\"Master.esm\"") != std::string::npos);
    CHECK(snap.find("\"masters\": [\"Master.esm\"]") != std::string::npos);
    CHECK(snap.find("\"localized\": true") != std::string::npos);
    (void)doc.World();

    std::printf("L5 OverrideInto\n");
    esper::Plugin author;
    esper::CreateEmptyPlugin(author, "Author.esp", false);
    // An unrelated first master shifts every master index, so FormIDs that
    // weren't rewritten into Author.esp's space would point at the wrong plugin.
    char perrBuf[256] = {};
    CHECK(esper::EnsureMaster(author, "Other.esm", perrBuf, sizeof(perrBuf)) == 0);
    // The winner's keywords point into Master.esm and Mod.esp: both become masters, in load order.
    esper::RecordNode* n1 = doc.OverrideInto(author, armorA, &err);
    CHECK(n1 != nullptr);
    if (!n1) std::printf("  %s\n", err.c_str());
    CHECK((author.masters == std::vector<std::string>{ "Other.esm", "Master.esm", "Mod.esp" }));
    // A localized winner into a non-localized plugin: the name becomes inline text.
    esper::RecordNode* n2 = doc.OverrideInto(author, Key("Master.esm", fx.armorLoc), &err);
    CHECK(n2 != nullptr);
    // Asking again returns the existing override.
    CHECK(doc.OverrideInto(author, armorA, &err) == author.index.at(n1 ? n1->header.formId : 0));
    // FormIDs the schema can't see: refused, and nothing is added.
    const size_t before = author.index.size();
    CHECK(doc.OverrideInto(author, Key("Master.esm", fx.armorRaw), &err) == nullptr);
    CHECK(err.find("aren't all visible") != std::string::npos);
    CHECK(author.index.size() == before);
    // Child records are refused up front.
    CHECK(doc.OverrideInto(author, Key("Master.esm", 0x123), &err) == nullptr);

    Save(author, fx.dir / "Data" / "Author.esp");
    esper::Plugin back;
    char perr[256] = {};
    CHECK(back.Load((fx.dir / "Data" / "Author.esp").string(), perr, sizeof(perr)));
    back.filename = "Author.esp";
    CHECK((back.masters == std::vector<std::string>{ "Other.esm", "Master.esm", "Mod.esp" }));
    size_t armors = 0;
    for (auto& [id, node] : back.index) {
        if (node->header.sig != kArmo) continue;
        ++armors;
        esper::Record rec;
        CHECK(esper::Record::Decode(back, *node, rec));
        CHECK(rec.RawCount() == 0);
        esper::view::Armor armor(rec);
        const esper::FormKey self = back.FormIdToKey(id);
        CHECK(self.plugin == "Master.esm");
        if (armor.EditorId() == std::string("ArmorA")) {
            CHECK(armor.FullName()->text == "Armor A (modded)");
            const auto kws = armor.Keywords();
            CHECK(kws && kws->size() == 2);
            if (kws && kws->size() == 2) {
                const esper::FormKey ka = back.FormIdToKey((*kws)[0].id.raw);
                const esper::FormKey kb = back.FormIdToKey((*kws)[1].id.raw);
                CHECK(ka.plugin == "Master.esm" && ka.localId == (fx.kwA & 0xFFFFFF));
                CHECK(kb.plugin == "Mod.esp" && kb.localId == (fx.kwB & 0xFFFFFF));
            }
        } else {
            CHECK(armor.EditorId() == std::string("ArmorLoc"));
            CHECK(armor.FullName()->text == "Localized Armor");
            const auto kws = armor.Keywords();
            CHECK(kws && kws->size() == 1 && back.FormIdToKey((*kws)[0].id.raw).plugin == "Master.esm");
        }
    }
    CHECK(armors == 2);

    fs::remove_all(fx.dir);
    if (g_failures == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL (%d check(s) failed)\n", g_failures);
    return 1;
}
