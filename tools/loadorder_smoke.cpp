// ---------------------------------------------------------------------------
// loadorder-smoke — LoadOrderDocument over a real Data folder.
//
//   loadorder-smoke <DataFolder> [--strings <dir>] [--plugins-txt <file>]
//
// Without --plugins-txt the vanilla load order is used (implicit masters plus
// installed Creation Club content). --strings points at a folder holding loose
// string tables (vanilla ships them in BSAs, which ESPER doesn't read).
// Prints load time, counts, a search with name resolution and link following,
// an OverrideInto round trip, the world index, and the snapshot size.
// Opt-in target: configure with -DESPER_BUILD_TOOLS=ON.
// ---------------------------------------------------------------------------
#include <ESPER/LoadOrder.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace esper;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: loadorder-smoke <DataFolder> [--strings <dir>] [--plugins-txt <file>]\n");
        return 2;
    }
    LoadOrderOptions opt;
    opt.dataFolder = argv[1];
    std::string stringsDir;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--strings") stringsDir = argv[i + 1];
        else if (a == "--plugins-txt") opt.pluginsTxt = argv[i + 1];
    }
    const fs::path tmp = fs::temp_directory_path() / "esper_loadorder_smoke";
    fs::create_directories(tmp);
    if (opt.pluginsTxt.empty()) {
        opt.pluginsTxt = (tmp / "plugins.txt").string();
        std::ofstream(opt.pluginsTxt) << "";          // implicit masters + Skyrim.ccc only
    }
    if (!stringsDir.empty()) {
        const std::string data = opt.dataFolder;
        opt.fetch = [data, stringsDir](const std::string& rel, std::vector<uint8_t>& out) {
            for (const fs::path& p : { fs::path(data) / rel,
                                       fs::path(stringsDir) / fs::path(rel).filename() }) {
                std::ifstream f(p, std::ios::binary);
                if (!f) continue;
                out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                return true;
            }
            return false;
        };
    }

    const auto t0 = std::chrono::steady_clock::now();
    LoadOrderDocument doc;
    std::string err;
    if (!doc.Load(opt, &err)) {
        std::printf("FAIL  load: %s\n", err.c_str());
        return 1;
    }
    const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    size_t loaded = doc.Plugins().size(), missing = 0;
    for (const PluginListing& l : doc.Listing()) missing += l.missing;
    std::printf("loaded %zu plugin(s) in %.0f ms (%zu listed missing, %zu warning(s))\n",
                loaded, loadMs, missing, doc.Warnings().size());
    for (const char* sig : { "NPC_", "ARMO", "WEAP", "CELL", "REFR" })
        std::printf("  winners %s: %zu\n", sig, doc.Enumerate(Sig4(sig)).size());

    int failures = 0;
    const auto hits = doc.Search(Sig4("NPC_"), "Lydia");
    std::printf("search NPC_ \"Lydia\": %zu hit(s)\n", hits.size());
    for (const ResolvedRecord* r : hits) {
        std::printf("  %s  %s  \"%s\"\n", r->key.ToString().c_str(), doc.EditorId(*r).c_str(),
                    doc.Name(*r).c_str());
        auto d = doc.Decode(*r, &err);
        if (!d) { std::printf("  FAIL decode: %s\n", err.c_str()); ++failures; continue; }
        view::Npc npc(d->record);
        if (auto race = npc.Race()) {
            const ResolvedRecord* rr = doc.Follow(*r, *race);
            std::printf("    race -> %s \"%s\"\n", rr ? doc.EditorId(*rr).c_str() : "(unresolved)",
                        rr ? doc.Name(*rr).c_str() : "");
            if (!rr) ++failures;
        }
    }

    // OverrideInto: copy a few vanilla records into a fresh plugin, save, reload.
    Plugin author;
    CreateEmptyPlugin(author, "EsperSmoke.esp", false);
    size_t copied = 0, refused = 0;
    std::string firstRefusal;
    for (const char* sig : { "WEAP", "ARMO", "NPC_", "KYWD" }) {
        size_t n = 0;
        for (const ResolvedRecord* r : doc.Enumerate(Sig4(sig))) {
            if (n++ >= 25) break;
            if (doc.OverrideInto(author, r->key, &err)) ++copied;
            else { ++refused; if (firstRefusal.empty()) firstRefusal = err; }
        }
    }
    char perr[512] = {};
    const fs::path out = tmp / "EsperSmoke.esp";
    if (!FinalizePlugin(author, perr, sizeof(perr)) || !SavePlugin(author, out.string(), perr, sizeof(perr))) {
        std::printf("FAIL  save: %s\n", perr);
        ++failures;
    }
    Plugin back;
    size_t reDecoded = 0, rawAfter = 0;
    if (back.Load(out.string(), perr, sizeof(perr))) {
        back.filename = "EsperSmoke.esp";
        for (auto& [id, node] : back.index) {
            Record rec;
            if (Record::Decode(back, *node, rec)) { ++reDecoded; rawAfter += rec.RawCount(); }
        }
    } else {
        std::printf("FAIL  reload: %s\n", perr);
        ++failures;
    }
    std::printf("OverrideInto: %zu copied, %zu refused; saved + reloaded %zu record(s), %zu raw; masters:",
                copied, refused, reDecoded, rawAfter);
    for (const std::string& m : back.masters) std::printf(" %s", m.c_str());
    std::printf("\n");
    if (!firstRefusal.empty()) std::printf("  first refusal: %s\n", firstRefusal.c_str());
    if (reDecoded != copied) ++failures;

    const auto t1 = std::chrono::steady_clock::now();
    const WorldIndex& world = doc.World();
    std::vector<const ResolvedRecord*> worlds;
    world.EnumerateWorldspaces(doc.Index(), worlds);
    std::printf("world index: %zu worldspace(s) in %.0f ms\n", worlds.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
    std::printf("snapshot: %zu bytes\n", doc.SnapshotJson().size());

    fs::remove_all(tmp);
    std::printf("%s\n", failures ? "LOADORDER SMOKE: FAIL" : "LOADORDER SMOKE: PASS");
    return failures ? 1 : 0;
}
