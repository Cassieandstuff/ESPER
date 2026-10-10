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

esper::view::Armor Armo(esper::Record& r) { return *esper::view::As<esper::view::Armor>(r); }
esper::view::Keyword Kywd(esper::Record& r) { return *esper::view::As<esper::view::Keyword>(r); }

// A new record of `sig` in `p`, filled by `fill` through the schema engine.
template <class Fill>
uint32_t AddRecord(esper::Plugin& p, uint32_t sig, Fill&& fill) {
    auto node = esper::CreateRecord(p, sig);
    if (!node) { std::printf("  CreateRecord: %s\n", node.error().message.c_str()); return 0; }
    auto rec = esper::Record::Create(p, sig);
    if (!rec) { std::printf("  Create: %s\n", rec.error().message.c_str()); return 0; }
    fill(*rec);
    if (auto ok = esper::Record::Commit(p, **node, *rec); !ok) std::printf("  Commit: %s\n", ok.error().message.c_str());
    return (*node)->header.formId;
}

void Save(esper::Plugin& p, const fs::path& path) {
    if (auto ok = esper::SavePlugin(p, path.string()); !ok)    // finalizes the edited plugin
        std::printf("  save %s: %s\n", path.string().c_str(), ok.error().message.c_str());
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
    fx.kwA = AddRecord(master, kKywd, [](esper::Record& r) { Kywd(r).SetEditorId("KwA"); });
    fx.armorA = AddRecord(master, kArmo, [&](esper::Record& r) {
        Armo(r).SetEditorId("ArmorA");
        Armo(r).SetFullName({ 1, {} });
        Armo(r).SetKeywords({ { { fx.kwA } } });
    });
    fx.armorLoc = AddRecord(master, kArmo, [&](esper::Record& r) {
        Armo(r).SetEditorId("ArmorLoc");
        Armo(r).SetFullName({ 2, {} });
        Armo(r).SetKeywords({ { { fx.kwA } } });
    });
    // A record with a subrecord the schema doesn't know: FormIDs not all visible.
    fx.armorRaw = AddRecord(master, kArmo, [](esper::Record& r) { Armo(r).SetEditorId("ArmorRaw"); });
    {
        esper::ByteWriter w;
        const char edid[] = "ArmorRaw";
        esper::EmitSubrecord(w, esper::Sig4("EDID"), edid, sizeof(edid));
        const uint8_t junk[4] = { 1, 2, 3, 4 };
        esper::EmitSubrecord(w, esper::Sig4("ZZZZ"), junk, 4);
        CHECK(esper::SetRecordPayload(master, *master.Find(fx.armorRaw), std::move(w.buf)));
    }
    Save(master, fx.dir / "Data" / "Master.esm");
    WriteFile(fx.dir / "Data" / "Strings" / "Master_English.STRINGS",
              StringsFile({ { 1, "Armor A (localized)" }, { 2, "Localized Armor" } }));

    // Mod.esp — overrides ArmorA (new inline name, an extra keyword) and adds KwB.
    esper::Plugin mod;
    esper::CreateEmptyPlugin(mod, "Mod.esp", false);
    const uint32_t kwAInMod = esper::MakeRef(mod, esper::FormKey{ "Master.esm", fx.kwA & 0xFFFFFF }).value_or(0);
    const uint32_t armorAInMod = esper::MakeRef(mod, esper::FormKey{ "Master.esm", fx.armorA & 0xFFFFFF }).value_or(0);
    fx.kwB = AddRecord(mod, kKywd, [](esper::Record& r) { Kywd(r).SetEditorId("KwB"); });
    {
        esper::Record rec = esper::Record::Create(mod, kArmo).value();
        Armo(rec).SetEditorId("ArmorA");
        Armo(rec).SetFullName({ 0, "Armor A (modded)" });
        Armo(rec).SetKeywords({ { { kwAInMod } }, { { fx.kwB } } });
        esper::RecHeader h{};
        h.sig = kArmo;
        h.formId = armorAInMod;
        h.version = 44;
        CHECK(esper::InsertRecord(mod, h, rec.Encode()));
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
    if (auto loaded = doc.Load(opt); !loaded) {
        std::printf("load failed: %s\n", loaded.error().message.c_str());
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
            CHECK(!esper::view::As<esper::view::Keyword>(d->record));   // wrong type: no view
            auto armor = *esper::view::As<esper::view::Armor>(d->record);
            CHECK(doc.Text(*d, *armor.FullName()) == "Armor A (modded)");
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

    // A localized FULL resolves through the table the schema gives the field.
    if (origin) {
        auto d = doc.Decode(*origin);
        CHECK(d && doc.Text(*d, *esper::view::As<esper::view::Armor>(d->record)->FullName()) == "Armor A (localized)");
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
    CHECK(esper::EnsureMaster(author, "Other.esm") == 0);
    // The winner's keywords point into Master.esm and Mod.esp: both become masters, in load order.
    const auto r1 = doc.OverrideInto(author, armorA);
    CHECK(r1.has_value());
    if (!r1) std::printf("  %s\n", r1.error().message.c_str());
    esper::RecordNode* n1 = r1.value_or(nullptr);
    CHECK((author.masters == std::vector<std::string>{ "Other.esm", "Master.esm", "Mod.esp" }));
    // A localized winner into a non-localized plugin: the name becomes inline text.
    CHECK(doc.OverrideInto(author, Key("Master.esm", fx.armorLoc)).has_value());
    // Asking again returns the existing override.
    CHECK(doc.OverrideInto(author, armorA).value_or(nullptr) == author.Find(n1 ? n1->header.formId : 0));
    // FormIDs the schema can't see: refused, and nothing is added.
    const size_t before = author.index.size();
    const auto refused = doc.OverrideInto(author, Key("Master.esm", fx.armorRaw));
    CHECK(!refused && refused.error().message.find("aren't all visible") != std::string::npos);
    CHECK(author.index.size() == before);
    // Child records are refused up front.
    CHECK(!doc.OverrideInto(author, Key("Master.esm", 0x123)));

    Save(author, fx.dir / "Data" / "Author.esp");
    esper::Plugin back;
    CHECK(back.Load((fx.dir / "Data" / "Author.esp").string()));
    CHECK(back.filename == "Author.esp");                      // Load names the plugin
    CHECK((back.masters == std::vector<std::string>{ "Other.esm", "Master.esm", "Mod.esp" }));
    size_t armors = 0;
    for (esper::RecordNode* node : back.Records(kArmo)) {
        ++armors;
        auto rec = esper::Record::Decode(back, *node);
        CHECK(rec.has_value());
        if (!rec) continue;
        CHECK(rec->RawCount() == 0);
        auto armor = *esper::view::As<esper::view::Armor>(*rec);
        const esper::FormKey self = back.FormIdToKey(node->header.formId);
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
    CHECK(back.Records().size() == back.index.size());

    std::printf("L6 FormKey text\n");
    {
        const auto k = esper::FormKey::Parse("013BBA:Skyrim.esm");
        CHECK(k && k->plugin == "Skyrim.esm" && k->localId == 0x013BBA);
        CHECK(k && k->ToString() == "013BBA:Skyrim.esm");
        const auto k2 = esper::FormKey::Parse("0x800:Mod.esl");
        CHECK(k2 && k2->plugin == "Mod.esl" && k2->localId == 0x800);
        CHECK(!esper::FormKey::Parse("Skyrim.esm"));                    // no colon
        CHECK(!esper::FormKey::Parse("12G4:Skyrim.esm"));               // not hex
        CHECK(!esper::FormKey::Parse("01013BBA:Skyrim.esm"));           // load-order byte
        CHECK(!esper::FormKey::Parse("013BBA:"));                       // no plugin
    }

    fs::remove_all(fx.dir);
    if (g_failures == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL (%d check(s) failed)\n", g_failures);
    return 1;
}
