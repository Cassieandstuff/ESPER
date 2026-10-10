// ---------------------------------------------------------------------------
// roundtrip_gate — the ESPER real-data gate.
//
// The unit tests prove the parser on synthetic fixtures; this proves it on the
// actual game masters. Two checks per plugin:
//   1. Parse with ZERO structural diagnostics.
//   2. Re-emit (SerializePlugin) and byte-compare against the original file —
//      an unmodified plugin must round-trip IDENTICALLY (compressed records
//      carry their raw bytes through, so this holds for them too).
//
// Opt-in target: configure with -DESPER_BUILD_TOOLS=ON.
//   roundtrip-gate <DataFolder>            gate the five vanilla masters
//   roundtrip-gate <plugin.es[mpl]> ...    gate specific plugin files
//   roundtrip-gate --schema [--baseline F] [--write-baseline F] <DataFolder | plugins>
//       the schema engine on real data; F is tools/gate-baseline.tsv
// ---------------------------------------------------------------------------
#include <ESPER.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace esper;

static int GateOne(const fs::path& path) {
    Plugin p;
    if (auto loaded = p.Load(path.string()); !loaded) {
        std::printf("FAIL  %s: %s\n", path.filename().string().c_str(), loaded.error().message.c_str());
        return 1;
    }

    std::map<std::string, size_t> bySig;
    for (const auto& [formId, node] : p.index) {
        (void)formId;
        char tag[5] = {};
        std::memcpy(tag, &node->header.sig, 4);
        ++bySig[tag];
    }

    std::printf("%-24s %10zu bytes  %8zu records  %2zu masters%s%s\n",
                p.filename.c_str(), p.buffer.size(), p.index.size(),
                p.masters.size(),
                p.isLight ? "  [light]" : "",
                p.localized ? "  [localized]" : "");

    // HEDR.numRecords rule probe: report the header's claim vs our counts so
    // the write path's recompute rule is empirical, not documentation-trusted.
    {
        size_t groups = 0;
        struct { void Count(const std::vector<Node>& ns, size_t& g) {
            for (const Node& n : ns)
                if (n.IsGroup()) { ++g; Count(n.group->children, g); }
        } } counter;
        counter.Count(p.tree, groups);
        std::printf("    HEDR.numRecords=%d  vs  records=%zu  records+groups=%zu\n",
                    p.hedrNumRecords, p.index.size(), p.index.size() + groups);
    }

    // Top signatures by count (the long tail is noise at gate level).
    std::vector<std::pair<std::string, size_t>> top(bySig.begin(), bySig.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::printf("    top sigs:");
    for (size_t i = 0; i < top.size() && i < 10; ++i)
        std::printf(" %s=%zu", top[i].first.c_str(), top[i].second);
    std::printf("  (%zu distinct)\n", top.size());

    // Roster coverage: which present signatures have no schema at all.
    std::vector<std::string> unhandled;
    for (const auto& [tag, count] : bySig) {
        (void)count;
        uint32_t s = 0;
        std::memcpy(&s, tag.data(), 4);
        if (!schema::Main().Find(s)) unhandled.push_back(tag);
    }
    if (unhandled.empty()) {
        std::printf("    roster: all %zu present signatures have a schema\n", bySig.size());
    } else {
        std::printf("    roster: %zu of %zu signatures without a schema:",
                    unhandled.size(), bySig.size());
        for (const std::string& u : unhandled) std::printf(" %s", u.c_str());
        std::printf("\n");
    }

    if (!p.diagnostics.empty()) {
        std::printf("    %zu DIAGNOSTIC(S):\n", p.diagnostics.size());
        for (size_t i = 0; i < p.diagnostics.size() && i < 20; ++i)
            std::printf("      @%llu: %s\n",
                        static_cast<unsigned long long>(p.diagnostics[i].offset),
                        p.diagnostics[i].message.c_str());
        return 1;
    }

    // Round-trip: re-emit and byte-compare against the original file.
    const auto emittedR = SerializePlugin(p);
    if (!emittedR) {
        std::printf("    FAIL  re-emit: %s\n", emittedR.error().message.c_str());
        return 1;
    }
    const std::vector<uint8_t>& emitted = *emittedR;
    if (emitted.size() != p.buffer.size()) {
        std::printf("    FAIL  round-trip size mismatch: original %zu, emitted %zu\n",
                    p.buffer.size(), emitted.size());
        return 1;
    }
    if (std::memcmp(emitted.data(), p.buffer.data(), emitted.size()) != 0) {
        size_t at = 0;
        while (at < emitted.size() && emitted[at] == p.buffer[at]) ++at;
        std::printf("    FAIL  round-trip byte mismatch at offset %zu "
                    "(original 0x%02X, emitted 0x%02X)\n",
                    at, p.buffer[at], emitted[at]);
        return 1;
    }
    std::printf("    round-trip: byte-identical\n");

    return 0;
}

// ── Schema gate: the schema-driven engine (esper::Record) on real data ───────
//
// For every record whose sig the schema covers:
//   1. decode succeeds;
//   2. encode reproduces the payload, both verbatim and with every modeled
//      subrecord forcibly re-encoded (proves each layout is exact);
//   3. typed links: every `to:`-declared FormID that resolves within the gated
//      plugins points at a record of a declared type;
//   4. no regression against the baseline (tools/gate-baseline.tsv): per sig,
//      no more raw subrecords, no fewer visible FormIDs, no fewer remap-safe
//      records. The baseline only applies to the plugins it was written from
//      (names and sizes are recorded in it).
// 1, 2, 3-mismatch and 4 fail the gate.
struct SigStats {
    size_t records = 0, subs = 0, raw = 0, formIds = 0, remapSafe = 0;
    size_t decodeFail = 0, verbatimFail = 0, forcedFail = 0;
    size_t linksChecked = 0, linksUnresolved = 0, linksWrong = 0;
    std::map<std::string, size_t> refuseWhy;
    std::string firstProblem;
};

struct Baseline {
    std::vector<std::string> plugins;            // "name size"
    std::map<std::string, SigStats> sigs;        // records, raw, formIds, remapSafe
};

static std::vector<std::string> PluginIds(const std::vector<fs::path>& targets) {
    std::vector<std::string> ids;
    std::error_code ec;
    for (const fs::path& t : targets)
        ids.push_back(t.filename().string() + " " + std::to_string(fs::file_size(t, ec)));
    return ids;
}

static bool ReadBaseline(const fs::path& path, Baseline& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("# plugin ", 0) == 0) { out.plugins.push_back(line.substr(9)); continue; }
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string sig;
        SigStats s;
        if (in >> sig >> s.records >> s.raw >> s.formIds >> s.remapSafe) out.sigs[sig] = s;
    }
    return true;
}

static bool WriteBaseline(const fs::path& path, const std::vector<fs::path>& targets,
                          const std::map<std::string, SigStats>& stats) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "# roundtrip-gate --schema baseline: per record type, the schema's coverage on these\n"
         "# plugins. The gate fails when raw grows or formIds/remapSafe shrink. Regenerate with\n"
         "# --write-baseline after a change that improves coverage (and commit it).\n";
    for (const std::string& id : PluginIds(targets)) f << "# plugin " << id << "\n";
    f << "# sig\trecords\traw\tformIds\tremapSafe\n";
    for (const auto& [sig, st] : stats)
        f << sig << "\t" << st.records << "\t" << st.raw << "\t" << st.formIds << "\t" << st.remapSafe << "\n";
    return static_cast<bool>(f);
}

static int SchemaGate(const std::vector<fs::path>& targets, const std::string& baselinePath,
                      const std::string& writeBaselinePath) {
    std::vector<std::unique_ptr<Plugin>> plugins;
    for (const fs::path& t : targets) {
        auto p = std::make_unique<Plugin>();
        if (auto loaded = p->Load(t.string()); !loaded) {
            std::printf("FAIL  %s: %s\n", t.filename().string().c_str(), loaded.error().message.c_str());
            return 1;
        }
        plugins.push_back(std::move(p));
    }

    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    // FormKey → record sig across the gated plugins (overrides share the sig).
    std::unordered_map<std::string, uint32_t> sigOf;
    for (const auto& p : plugins)
        for (const auto& [raw, node] : p->index) {
            const FormKey k = p->FormIdToKey(raw);
            sigOf[lower(k.plugin) + "|" + std::to_string(k.localId & 0xFFFFFFu)] = node->header.sig;
        }

    const schema::SchemaSet& set = schema::Main();
    std::map<std::string, SigStats> stats;

    for (const auto& p : plugins) {
        for (const auto& [raw, node] : p->index) {
            const uint32_t sig = node->header.sig;
            if (!set.Find(sig)) continue;
            SigStats& st = stats[SigName(sig)];
            auto note = [&](const std::string& what) {
                if (st.firstProblem.empty())
                    st.firstProblem = std::format("{} {:08X}: {}", p->filename, raw, what);
            };
            ++st.records;
            const auto pr = node->Payload();
            auto decoded = pr ? Record::Decode(*p, *node) : Result<Record>(std::unexpected(pr.error()));
            if (!decoded) {
                ++st.decodeFail;
                note("decode failed: " + decoded.error().message);
                continue;
            }
            const std::span<const uint8_t> payload = *pr;
            Record& rec = *decoded;
            st.subs += rec.SubrecordCount();
            st.raw  += rec.RawCount();
            const std::vector<uint8_t> want(payload.begin(), payload.end());
            if (rec.Encode(false) != want) { ++st.verbatimFail; note("verbatim encode differs"); }
            if (rec.Encode(true) != want)  { ++st.forcedFail;   note("forced re-encode differs"); }

            if (const auto safe = rec.CheckRemapSafe()) ++st.remapSafe;
            else ++st.refuseWhy[safe.error().message];
            rec.ForEachFormId([&](FormId fid, std::span<const uint32_t> to) {
                if (fid.raw == 0) return;
                ++st.formIds;
                if (to.empty()) return;
                ++st.linksChecked;
                const FormKey k = p->FormIdToKey(fid.raw);
                const auto it = sigOf.find(lower(k.plugin) + "|" + std::to_string(k.localId & 0xFFFFFFu));
                if (it == sigOf.end()) { ++st.linksUnresolved; return; }
                if (std::find(to.begin(), to.end(), it->second) == to.end()) {
                    ++st.linksWrong;
                    note(std::format("link {:08X} points at a {}", fid.raw, SigName(it->second)));
                }
            });
        }
    }

    Baseline base;
    bool useBase = false;
    if (!baselinePath.empty()) {
        if (!ReadBaseline(baselinePath, base)) {
            std::printf("FAIL  can't read baseline %s\n", baselinePath.c_str());
            return 1;
        }
        useBase = base.plugins == PluginIds(targets);
        if (!useBase)
            std::printf("note: baseline %s was written from other plugins (names/sizes differ); "
                        "regression check skipped\n", baselinePath.c_str());
    }

    int failures = 0;
    size_t totRecords = 0, totSubs = 0, totRaw = 0, totIds = 0, totSafe = 0, totLinks = 0, totWrong = 0;
    std::printf("\nSchema gate (esper::Record, schema %s, language %s):\n",
                set.schemaVersion, set.languageVersion);
    for (const auto& [sig, st] : stats) {
        std::string regress;
        if (useBase) {
            const auto b = base.sigs.find(sig);
            if (b != base.sigs.end()) {
                if (st.raw > b->second.raw)
                    regress += std::format(" raw {} > baseline {};", st.raw, b->second.raw);
                if (st.formIds < b->second.formIds)
                    regress += std::format(" FormIDs {} < baseline {};", st.formIds, b->second.formIds);
                if (st.remapSafe < b->second.remapSafe)
                    regress += std::format(" remap-safe {} < baseline {};", st.remapSafe, b->second.remapSafe);
            }
        }
        const bool bad = st.decodeFail || st.verbatimFail || st.forcedFail || st.linksWrong || !regress.empty();
        failures += bad ? 1 : 0;
        totRecords += st.records; totSubs += st.subs; totRaw += st.raw; totIds += st.formIds;
        totSafe += st.remapSafe; totLinks += st.linksChecked; totWrong += st.linksWrong;
        std::printf("  %s %s  %zu record(s), %zu subrecord(s), %zu raw; %zu FormID(s); %zu remap-safe\n",
                    bad ? "FAIL" : "ok  ", sig.c_str(), st.records, st.subs, st.raw, st.formIds, st.remapSafe);
        if (st.decodeFail || st.verbatimFail || st.forcedFail)
            std::printf("         decode-fail %zu, verbatim-diff %zu, forced-diff %zu\n",
                        st.decodeFail, st.verbatimFail, st.forcedFail);
        for (const auto& [why, n] : st.refuseWhy)
            std::printf("           refuses %zu: %s\n", n, why.c_str());
        if (st.linksChecked)
            std::printf("         links: %zu checked, %zu unresolved, %zu wrong\n",
                        st.linksChecked, st.linksUnresolved, st.linksWrong);
        if (!regress.empty()) std::printf("         REGRESSION:%s\n", regress.c_str());
        if (!st.firstProblem.empty()) std::printf("         first problem: %s\n", st.firstProblem.c_str());
    }
    std::printf("\ntotal: %zu record(s), %zu subrecord(s), %.2f%% modeled; %zu FormID(s); "
                "%zu remap-safe (%.2f%%); %zu link(s) checked, %zu wrong\n",
                totRecords, totSubs,
                totSubs ? 100.0 * static_cast<double>(totSubs - totRaw) / static_cast<double>(totSubs) : 100.0,
                totIds, totSafe,
                totRecords ? 100.0 * static_cast<double>(totSafe) / static_cast<double>(totRecords) : 100.0,
                totLinks, totWrong);

    if (!writeBaselinePath.empty()) {
        if (failures) {
            std::printf("not writing the baseline: the gate failed\n");
        } else if (!WriteBaseline(writeBaselinePath, targets, stats)) {
            std::printf("FAIL  can't write baseline %s\n", writeBaselinePath.c_str());
            return 1;
        } else {
            std::printf("wrote baseline %s\n", writeBaselinePath.c_str());
        }
    }
    std::printf("%s\n", failures ? "SCHEMA GATE: FAIL" : "SCHEMA GATE: PASS");
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    bool schemaMode = false;
    std::string baselinePath, writeBaselinePath;
    while (argc >= 2 && std::string(argv[1]).rfind("--", 0) == 0) {
        const std::string opt = argv[1];
        if (opt == "--schema") {
            schemaMode = true;
        } else if ((opt == "--baseline" || opt == "--write-baseline") && argc >= 3) {
            (opt == "--baseline" ? baselinePath : writeBaselinePath) = argv[2];
            --argc;
            ++argv;
        } else {
            std::printf("unknown option %s\n", opt.c_str());
            return 2;
        }
        --argc;
        ++argv;
    }
    if (argc < 2) {
        std::printf("usage: roundtrip-gate <DataFolder>\n"
                    "       roundtrip-gate <plugin.esm|esp|esl> ...\n"
                    "       roundtrip-gate --schema [--baseline F] [--write-baseline F] <DataFolder | plugins>\n");
        return 2;
    }

    std::vector<fs::path> targets;
    std::error_code ec;
    const fs::path first(argv[1]);
    if (argc == 2 && fs::is_directory(first, ec)) {
        static const char* kVanilla[] = {
            "Skyrim.esm", "Update.esm", "Dawnguard.esm",
            "HearthFires.esm", "Dragonborn.esm"
        };
        for (const char* name : kVanilla) {
            const fs::path p = first / name;
            if (fs::exists(p, ec)) targets.push_back(p);
            else std::printf("skip  %s (not present)\n", name);
        }
    } else {
        for (int i = 1; i < argc; ++i) targets.emplace_back(argv[i]);
    }

    if (targets.empty()) {
        std::printf("FAIL  nothing to gate\n");
        return 1;
    }

    if (schemaMode) return SchemaGate(targets, baselinePath, writeBaselinePath);

    int failures = 0;
    for (const fs::path& t : targets) failures += GateOne(t);

    // Directory mode: also smoke the WorldIndex over the whole set — the
    // access patterns the editor's cell browser / terrain streamer will use.
    if (argc == 2 && fs::is_directory(first, ec) && failures == 0) {
        std::printf("\nWorldIndex smoke:\n");
        std::vector<std::unique_ptr<Plugin>> plugins;
        for (const fs::path& t : targets) {
            auto p = std::make_unique<Plugin>();
            if (auto loaded = p->Load(t.string()); !loaded) {
                std::printf("  FAIL reload %s: %s\n", t.filename().string().c_str(),
                            loaded.error().message.c_str());
                return 1;
            }
            plugins.push_back(std::move(p));
        }
        LoadOrderIndex index;
        index.Build(std::move(plugins));
        WorldIndex world;
        world.Build(index);

        std::vector<const ResolvedRecord*> worlds;
        index.EnumerateWinning(sig::WRLD, worlds);
        std::printf("  %zu worldspace(s), %zu exterior cell(s), %zu interior cell(s)\n",
                    worlds.size(), world.ExteriorCellCount(),
                    world.InteriorCellCount());
        // Per-worldspace distribution (largest first) — sanity vs known data.
        {
            std::vector<std::pair<size_t, std::string>> byWorld;
            for (const ResolvedRecord* w : worlds) {
                std::vector<const CellEntry*> cells;
                world.EnumerateExterior(index, w->key, cells);
                if (cells.empty()) continue;
                std::string name = w->key.ToString();
                if (auto rec = Record::Decode(*w->owner, *w->node))
                    if (auto edid = view::As<view::Worldspace>(*rec)->EditorId()) name = *edid;
                byWorld.emplace_back(cells.size(), name);
            }
            std::sort(byWorld.rbegin(), byWorld.rend());
            std::printf("  top worldspaces:");
            for (size_t i = 0; i < byWorld.size() && i < 6; ++i)
                std::printf(" %s=%zu", byWorld[i].second.c_str(), byWorld[i].first);
            std::printf("\n");
        }

        // Probe Tamriel (Skyrim.esm 00003C): Whiterun-area cell (4, -4) —
        // must exist, carry LAND with decodable heights, and yield refs.
        const FormKey tamriel{ "Skyrim.esm", 0x00003Cu };
        const CellEntry* cell = world.FindExterior(index, tamriel, 4, -4);
        if (!cell) {
            std::printf("  FAIL: Tamriel (4,-4) not found\n");
            return 1;
        }
        std::vector<PlacedRef> refs;
        WorldIndex::CollectPlacedRefs(*cell, refs);
        std::printf("  Tamriel (4,-4): %zu placed ref(s), LAND=%s\n",
                    refs.size(), cell->land ? "yes" : "no");
        if (!cell->land) {
            std::printf("  FAIL: Tamriel (4,-4) has no LAND\n");
            return 1;
        }
        auto landRec = Record::Decode(*cell->childOwner, *cell->land);
        if (!landRec) {
            std::printf("  FAIL: LAND decode (%s)\n", landRec.error().message.c_str());
            return 1;
        }
        const auto land = *view::As<view::Landscape>(*landRec);
        if (!land.HasHeights() || landRec->RawCount() != 0) {
            std::printf("  FAIL: LAND has no heights or raw subrecords\n");
            return 1;
        }
        std::printf("  LAND: VHGT offset %.1f, %zu base tex, %zu alpha layer(s), "
                    "%zu alpha cell list(s), colors=%s\n",
                    land.Heights().Offset().value_or(0.f), land.BaseTextures().size(),
                    land.AlphaTextures().size(), land.AlphaCells().size(),
                    land.HasColors() ? "yes" : "no");

        // Interior sanity: known interiors must be findable by EDID.
        {
            std::vector<const CellEntry*> interiors;
            world.EnumerateInteriors(interiors);
            size_t named = 0;
            bool sawBreezehome = false, sawBleakFalls = false;
            for (const CellEntry* ce : interiors) {
                const auto pl = ce->cell->Payload();
                if (!pl) continue;
                SubIterator sit(*pl);
                SubView sub;
                while (sit.Next(sub)) {
                    if (sub.sig == Sig4("EDID")) {
                        const std::string edid = ReadZString(sub);
                        if (!edid.empty()) ++named;
                        if (edid == "WhiterunBreezehome") sawBreezehome = true;
                        if (edid.rfind("BleakFallsBarrow", 0) == 0) sawBleakFalls = true;
                        break;
                    }
                }
            }
            std::printf("  interiors: %zu named; Breezehome=%s BleakFalls=%s\n",
                        named, sawBreezehome ? "yes" : "NO",
                        sawBleakFalls ? "yes" : "NO");
            if (!sawBreezehome || !sawBleakFalls) {
                std::printf("  FAIL: known interior cells missing from the index\n");
                return 1;
            }
        }

        // O(tiles) bulk enumeration — the TerrainStreamManager pattern.
        std::vector<const CellEntry*> tiles;
        world.EnumerateExterior(index, tamriel, tiles);
        size_t withLand = 0;
        for (const CellEntry* t : tiles)
            if (t->land) ++withLand;
        std::printf("  Tamriel: %zu cell(s), %zu with LAND\n", tiles.size(), withLand);
        if (tiles.empty() || withLand == 0) {
            std::printf("  FAIL: Tamriel enumeration empty\n");
            return 1;
        }
    }

    if (failures == 0) {
        std::printf("\nRESULT: PASS (%zu plugin(s), zero diagnostics)\n", targets.size());
        return 0;
    }
    std::printf("\nRESULT: FAIL (%d plugin(s) with problems)\n", failures);
    return 1;
}
