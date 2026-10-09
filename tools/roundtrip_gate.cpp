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
//   roundtrip-gate --schema <DataFolder>  the schema-driven engine on real data
//   roundtrip-gate <DataFolder>            gate the five vanilla masters
//   roundtrip-gate <plugin.es[mpl]> ...    gate specific plugin files
// ---------------------------------------------------------------------------
#include <ESPER/CtdaFunctions.h>
#include <ESPER/Plugin.h>
#include <ESPER/PluginFlags.h>
#include <ESPER/Record.h>
#include <ESPER/RecordsWorld.h>
#include <ESPER/SchemaRegistry.h>
#include <ESPER/Subrecords.h>
#include <ESPER/WorldIndex.h>
#include <ESPER/Writer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace esper;

static int GateOne(const fs::path& path) {
    Plugin p;
    p.filename = path.filename().string();
    char err[512] = {};
    if (!p.Load(path.string(), err, sizeof(err))) {
        std::printf("FAIL  %s: %s\n", p.filename.c_str(), err);
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

    // Roster coverage: which present signatures have no typed schema at all.
    std::vector<std::string> unhandled;
    for (const auto& [tag, count] : bySig) {
        (void)count;
        uint32_t s = 0;
        std::memcpy(&s, tag.data(), 4);
        if (!FindSchema(s)) unhandled.push_back(tag);
    }
    if (unhandled.empty()) {
        std::printf("    roster: all %zu present signatures are typed\n", bySig.size());
    } else {
        std::printf("    roster: %zu of %zu signatures un-typed:",
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
    std::vector<uint8_t> emitted;
    if (!SerializePlugin(p, emitted, err, sizeof(err))) {
        std::printf("    FAIL  re-emit: %s\n", err);
        return 1;
    }
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

    // Schema gate: every instance of every schema'd signature must decode →
    // encode back to its exact payload bytes. Also reports modeling coverage
    // (what fraction of subrecords the schemas semantically consume).
    struct SigStat {
        size_t instances = 0, failures = 0, totalSubs = 0, unknownSubs = 0;
        std::string firstWhy;
        uint32_t firstFailId = 0;
    };
    std::map<std::string, SigStat> stats;
    SchemaCtx ctx;
    ctx.localized = p.localized;

    for (const auto& [formId, node] : p.index) {
        const SchemaOps* ops = FindSchema(node->header.sig);
        if (!ops) continue;

        char perr[256] = {};
        const std::span<const uint8_t> payload = node->Payload(perr, sizeof(perr));
        if (payload.data() == nullptr) continue;   // decompression failure counted above

        char tag[5] = {};
        std::memcpy(tag, &node->header.sig, 4);
        SigStat& st = stats[tag];
        ++st.instances;

        size_t total = 0, unknown = 0;
        std::string why;
        if (!ops->roundtrip(payload, ctx, total, unknown, why)) {
            if (st.failures == 0) { st.firstWhy = why; st.firstFailId = formId; }
            ++st.failures;
        }
        st.totalSubs   += total;
        st.unknownSubs += unknown;
    }

    // CTDA classification coverage: scan every record's payload for condition
    // subrecords and tally how many use a function the remap table knows. The
    // unclassified indices are exactly the functions still to add.
    {
        size_t condTotal = 0, condKnown = 0;
        std::map<uint16_t, size_t> unknownFns;
        for (const auto& [formId, node] : p.index) {
            (void)formId;
            char perr[256] = {};
            const std::span<const uint8_t> payload = node->Payload(perr, sizeof(perr));
            if (payload.data() == nullptr) continue;
            SubIterator it(payload);
            SubView sub;
            while (it.Next(sub)) {
                if (sub.sig != Sig4("CTDA") || sub.size < 12) continue;
                uint16_t fn = 0;
                std::memcpy(&fn, sub.data + 8, 2);
                ++condTotal;
                CtdaFuncInfo info;
                if (LookupCtdaFunc(fn, info)) ++condKnown;
                else ++unknownFns[fn];
            }
        }
        if (condTotal) {
            std::printf("    conditions: %zu, %.1f%% classifiable",
                        condTotal, 100.0 * static_cast<double>(condKnown) /
                                       static_cast<double>(condTotal));
            // Top few unknown function indices by frequency.
            std::vector<std::pair<size_t, uint16_t>> uk;
            for (const auto& [fn, n] : unknownFns) uk.emplace_back(n, fn);
            std::sort(uk.rbegin(), uk.rend());
            if (!uk.empty()) {
                std::printf("  (top unknown fns:");
                for (size_t i = 0; i < uk.size() && i < 6; ++i)
                    std::printf(" %u\xC3\x97%zu", uk[i].second, uk[i].first);
                std::printf(")");
            }
            std::printf("\n");
        }
    }

    int schemaFailures = 0;
    for (const auto& [tag, st] : stats) {
        const double coverage =
            st.totalSubs ? 100.0 * static_cast<double>(st.totalSubs - st.unknownSubs) /
                               static_cast<double>(st.totalSubs)
                         : 100.0;
        std::printf("    schema %s: %zu instance(s), %zu failure(s), %.1f%% subs modeled\n",
                    tag.c_str(), st.instances, st.failures, coverage);
        if (st.failures) {
            std::printf("      first failure: %08X — %s\n", st.firstFailId,
                        st.firstWhy.c_str());
            ++schemaFailures;
        }
    }
    return schemaFailures ? 1 : 0;
}

// ── Schema gate: the schema-driven engine (esper::Record) on real data ───────
//
// For every record whose sig the generated schema covers:
//   1. decode succeeds;
//   2. encode reproduces the payload, both verbatim and with every modeled
//      subrecord forcibly re-encoded (proves each layout is exact);
//   3. raw subrecords: no more than the typed path's unknown-passthrough count;
//   4. FormIDs: when both paths consider the record remap-safe, they see the
//      same multiset of FormIDs;
//   5. typed links: every `to:`-declared FormID that resolves within the gated
//      plugins points at a record of a declared type.
// 1, 2, 3, 4-mismatch and 5-mismatch fail the gate; remap-safety differences
// between the paths are reported by reason.
static int SchemaGate(const std::vector<fs::path>& targets) {
    std::vector<std::unique_ptr<Plugin>> plugins;
    for (const fs::path& t : targets) {
        auto p = std::make_unique<Plugin>();
        char err[512] = {};
        if (!p->Load(t.string(), err, sizeof(err))) {
            std::printf("FAIL  %s: %s\n", t.filename().string().c_str(), err);
            return 1;
        }
        p->filename = t.filename().string();
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
    struct SigStats {
        size_t records = 0, subs = 0, raw = 0, oldUnknown = 0;
        size_t decodeFail = 0, verbatimFail = 0, forcedFail = 0, rawWorse = 0;
        size_t idsCompared = 0, idsMismatch = 0, newRefuses = 0, newAccepts = 0;
        size_t typedMissed = 0, typedMissedIds = 0;   // FormIDs only the schema sees
        std::string firstTypedMiss;
        size_t linksChecked = 0, linksUnresolved = 0, linksWrong = 0;
        std::map<std::string, size_t> refuseWhy;
        std::string firstProblem;
    };
    std::map<std::string, SigStats> stats;
    auto sigText = [](uint32_t sig) { std::string s(4, ' '); std::memcpy(s.data(), &sig, 4); return s; };

    for (const auto& p : plugins) {
        SchemaCtx oldCtx;
        oldCtx.localized = p->localized;
        for (const auto& [raw, node] : p->index) {
            const uint32_t sig = node->header.sig;
            if (!set.Find(sig)) continue;
            SigStats& st = stats[sigText(sig)];
            auto note = [&](const char* what) {
                if (st.firstProblem.empty()) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s %08X: %s", p->filename.c_str(), raw, what);
                    st.firstProblem = buf;
                }
            };
            ++st.records;
            char err[256] = {};
            const std::span<const uint8_t> payload = node->Payload(err, sizeof(err));
            Record rec;
            std::string why;
            if (payload.data() == nullptr || !Record::Decode(*p, *node, rec, &why)) {
                ++st.decodeFail;
                note("decode failed");
                continue;
            }
            st.subs += rec.SubrecordCount();
            st.raw  += rec.RawCount();
            const std::vector<uint8_t> want(payload.begin(), payload.end());
            if (rec.Encode(false) != want) { ++st.verbatimFail; note("verbatim encode differs"); }
            if (rec.Encode(true) != want)  { ++st.forcedFail;   note("forced re-encode differs"); }

            // Old typed path, for parity.
            const SchemaOps* ops = FindSchema(sig);
            size_t oldTotal = 0, oldUnknown = 0;
            std::string oldWhy;
            if (ops && ops->roundtrip(payload, oldCtx, oldTotal, oldUnknown, oldWhy)) {
                st.oldUnknown += oldUnknown;
                if (rec.RawCount() > oldUnknown) { ++st.rawWorse; note("more raw subrecords than the typed path"); }
            }
            std::vector<uint32_t> oldIds;
            std::vector<uint8_t> scratch;
            const bool oldSafe = ops && ops->remapIds(payload, oldCtx,
                [](uint32_t& id, void* c) { static_cast<std::vector<uint32_t>*>(c)->push_back(id); },
                &oldIds, scratch, oldWhy);
            std::string newWhy;
            const bool newSafe = rec.RemapSafe(&newWhy);
            std::vector<uint32_t> newIds;
            rec.ForEachFormId([&](uint32_t id, std::span<const uint32_t> to) {
                newIds.push_back(id);
                if (to.empty() || id == 0) return;
                ++st.linksChecked;
                const FormKey k = p->FormIdToKey(id);
                const auto it = sigOf.find(lower(k.plugin) + "|" + std::to_string(k.localId & 0xFFFFFFu));
                if (it == sigOf.end()) { ++st.linksUnresolved; return; }
                if (std::find(to.begin(), to.end(), it->second) == to.end()) {
                    ++st.linksWrong;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "link %08X points at a %s", id, sigText(it->second).c_str());
                    note(buf);
                }
            });
            if (oldSafe && newSafe) {
                // Null (0) is never remapped; the typed path's condition visitor
                // skips it while plain FormID rows include it, so compare non-null.
                std::erase(oldIds, 0u);
                std::erase(newIds, 0u);
                ++st.idsCompared;
                std::sort(oldIds.begin(), oldIds.end());
                std::sort(newIds.begin(), newIds.end());
                if (oldIds != newIds) {
                    std::vector<uint32_t> onlyNew, onlyOld;
                    std::set_difference(newIds.begin(), newIds.end(), oldIds.begin(), oldIds.end(),
                                        std::back_inserter(onlyNew));
                    std::set_difference(oldIds.begin(), oldIds.end(), newIds.begin(), newIds.end(),
                                        std::back_inserter(onlyOld));
                    // FormIDs only the schema sees are ones the typed path skipped
                    // (its opaque Blob rows); reported, not failed. A FormID only
                    // the typed path sees is one the schema would miss: a failure.
                    if (onlyOld.empty()) {
                        ++st.typedMissed;
                        st.typedMissedIds += onlyNew.size();
                        if (st.firstTypedMiss.empty()) {
                            char m[96];
                            std::snprintf(m, sizeof(m), "%s %08X: e.g. %08X", p->filename.c_str(), raw, onlyNew[0]);
                            st.firstTypedMiss = m;
                        }
                        continue;
                    }
                    ++st.idsMismatch;
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "FormID sets differ from the typed path (new-only %zu, e.g. %08X; "
                                  "typed-only %zu, e.g. %08X)",
                                  onlyNew.size(), onlyNew.empty() ? 0u : onlyNew[0],
                                  onlyOld.size(), onlyOld.empty() ? 0u : onlyOld[0]);
                    note(buf);
                }
            } else if (oldSafe && !newSafe) {
                ++st.newRefuses;
                ++st.refuseWhy[newWhy];
            } else if (!oldSafe && newSafe) {
                ++st.newAccepts;
            }
        }
    }

    int failures = 0;
    std::printf("\nSchema gate (esper::Record, schema %s, language %s):\n",
                set.schemaVersion, set.languageVersion);
    for (const auto& [sig, st] : stats) {
        const bool bad = st.decodeFail || st.verbatimFail || st.forcedFail || st.rawWorse ||
                         st.idsMismatch || st.linksWrong;
        failures += bad ? 1 : 0;
        std::printf("  %s %s  %zu record(s), %zu subrecord(s), %zu raw (typed path: %zu unknown)\n",
                    bad ? "FAIL" : "ok  ", sig.c_str(), st.records, st.subs, st.raw, st.oldUnknown);
        std::printf("         decode-fail %zu, verbatim-diff %zu, forced-diff %zu, raw-worse %zu\n",
                    st.decodeFail, st.verbatimFail, st.forcedFail, st.rawWorse);
        std::printf("         FormIDs: %zu compared, %zu mismatched; new refuses %zu, new accepts %zu\n",
                    st.idsCompared, st.idsMismatch, st.newRefuses, st.newAccepts);
        for (const auto& [why, n] : st.refuseWhy)
            std::printf("           refuses %zu: %s\n", n, why.c_str());
        if (st.typedMissed)
            std::printf("         typed path missed %zu FormID(s) in %zu record(s) (first: %s)\n",
                        st.typedMissedIds, st.typedMissed, st.firstTypedMiss.c_str());
        std::printf("         links: %zu checked, %zu unresolved, %zu wrong\n",
                    st.linksChecked, st.linksUnresolved, st.linksWrong);
        if (!st.firstProblem.empty()) std::printf("         first problem: %s\n", st.firstProblem.c_str());
    }
    std::printf("%s\n", failures ? "SCHEMA GATE: FAIL" : "SCHEMA GATE: PASS");
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    bool schemaMode = false;
    if (argc >= 2 && std::string(argv[1]) == "--schema") {
        schemaMode = true;
        --argc;
        ++argv;
    }
    if (argc < 2) {
        std::printf("usage: roundtrip-gate [--schema] <DataFolder>\n"
                    "       roundtrip-gate [--schema] <plugin.esm|esp|esl> ...\n");
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

    if (schemaMode) return SchemaGate(targets);

    int failures = 0;
    for (const fs::path& t : targets) failures += GateOne(t);

    // Directory mode: also smoke the WorldIndex over the whole set — the
    // access patterns the editor's cell browser / terrain streamer will use.
    if (argc == 2 && fs::is_directory(first, ec) && failures == 0) {
        std::printf("\nWorldIndex smoke:\n");
        std::vector<std::unique_ptr<Plugin>> plugins;
        for (const fs::path& t : targets) {
            auto p = std::make_unique<Plugin>();
            p->filename = t.filename().string();
            char err[512] = {};
            if (!p->Load(t.string(), err, sizeof(err))) {
                std::printf("  FAIL reload %s: %s\n", p->filename.c_str(), err);
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
                char perr[256] = {};
                WrldRec wr;
                SchemaCtx wctx;
                wctx.localized = w->owner->localized;
                std::string name = w->key.ToString();
                const auto pl = w->node->Payload(perr, sizeof(perr));
                if (pl.data() && DecodeRecord(kWrldSchema, pl, wctx, wr) && wr.editorId)
                    name = *wr.editorId;
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
        char lerr[256] = {};
        LandRec land;
        SchemaCtx lctx;
        lctx.localized = cell->childOwner && cell->childOwner->localized;
        const auto payload = cell->land->Payload(lerr, sizeof(lerr));
        if (payload.data() == nullptr ||
            !DecodeRecord(kLandSchema, payload, lctx, land) || !land.heights) {
            std::printf("  FAIL: LAND decode (%s)\n", lerr);
            return 1;
        }
        std::printf("  LAND: VHGT offset %.1f, %zu base tex, %zu alpha layer(s), "
                    "colors=%s\n",
                    land.heights->offset, land.baseTextures.size(),
                    land.alphaTextures.size(), land.colors ? "yes" : "no");

        // Interior sanity: known interiors must be findable by EDID.
        {
            std::vector<const CellEntry*> interiors;
            world.EnumerateInteriors(interiors);
            size_t named = 0;
            bool sawBreezehome = false, sawBleakFalls = false;
            for (const CellEntry* ce : interiors) {
                char perr[256] = {};
                const auto pl = ce->cell->Payload(perr, sizeof(perr));
                if (!pl.data()) continue;
                SubIterator sit(pl);
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
