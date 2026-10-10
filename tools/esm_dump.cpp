// ---------------------------------------------------------------------------
// esm-dump — read-only research dumper on the ESPER read path.
//
// Purpose: ground-truth inspection of shipped game data — Story Manager node
// linkage (SMBN/SMQN/SMEN + PNAM parent graph), quest alias/package sections,
// and CTDA condition rows (named via the schema's ctdaFunctions table). Answers
// the cutscene-trigger research questions from real bytes instead of inference.
//
// Read-only: never writes a plugin. Opt-in target (ESPER_BUILD_TOOLS).
//
//   esm-dump <plugin>  grup [--depth N]
//       Walk the GRUP tree: signature/type/child-count per group. Reveals
//       whether a record family (e.g. SM nodes) nests or lives flat.
//
//   esm-dump <plugin>  sig SIG [--edid SUBSTR] [--limit N]
//       Dump every record of signature SIG (optionally filtered by an EDID
//       substring, case-insensitive), full subrecord breakdown.
//
//   esm-dump <plugin>  form HEXID
//       Dump one record by its on-disk FormID (hex, e.g. 0x0004E514).
// ---------------------------------------------------------------------------
#include <ESPER.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace esper;

static std::string TagOf(uint32_t sig) {
    char t[5] = {};
    std::memcpy(t, &sig, 4);
    for (char& c : t) if (c && !std::isprint(static_cast<unsigned char>(c))) c = '.';
    return std::string(t, 4);
}

static uint32_t SigFromArg(const char* s) {
    char b[4] = {' ', ' ', ' ', ' '};
    for (int i = 0; i < 4 && s[i]; ++i) b[i] = s[i];
    uint32_t v = 0;
    std::memcpy(&v, b, 4);
    return v;
}

static bool LooksText(const uint8_t* d, uint32_t n) {
    if (n < 2) return false;
    // Printable run, optionally null-terminated.
    for (uint32_t i = 0; i + 1 < n; ++i)
        if (!std::isprint(static_cast<unsigned char>(d[i]))) return false;
    return d[n - 1] == 0 || std::isprint(static_cast<unsigned char>(d[n - 1]));
}

static void PrintHex(const uint8_t* d, uint32_t n, uint32_t cap = 48) {
    const uint32_t m = std::min(n, cap);
    for (uint32_t i = 0; i < m; ++i) std::printf("%02X ", d[i]);
    if (n > cap) std::printf("... (%u bytes)", n);
}

static const char* FillTypeName(uint32_t sig) {
    switch (sig) {
        case Sig4("ALFR"): return "ForcedReference";
        case Sig4("ALFA"): return "FindMatchingRefFromEvent/NearAlias";
        case Sig4("ALFE"): return "FromEvent";
        case Sig4("ALFD"): return "FromEventData(which-ref)";
        case Sig4("ALFI"): return "ForcedIntoAlias";
        case Sig4("ALCO"): return "CreatedObject";
        case Sig4("ALEQ"): return "ExternalQuestAlias";
        case Sig4("ALUA"): return "UniqueActor";
        case Sig4("ALCA"): return "CreatedAlias(target)";
        case Sig4("ALRT"): return "RefTypeFilter";
        default: return nullptr;
    }
}

// One CTDA row, raw: the byte layout of schema/groups/Condition.yaml, with the
// function's name and parameter kinds from the schema's ctdaFunctions table.
static void DumpCtda(const uint8_t* d, uint32_t n, const Plugin& p) {
    if (n < 28) { std::printf("      (short CTDA %u)\n", n); return; }
    auto u32 = [&](uint32_t at) { uint32_t v = 0; std::memcpy(&v, d + at, 4); return v; };
    const uint8_t  op     = d[0];
    const uint32_t cmp    = u32(4);
    uint16_t       fn     = 0;
    std::memcpy(&fn, d + 8, 2);
    const uint32_t p1 = u32(12), p2 = u32(16), runOn = u32(20), ref = n >= 28 ? u32(24) : 0;
    const bool useGlobal = (op & 0x04) != 0, aliases = (op & 0x02) != 0, isOr = (op & 0x01) != 0;

    const schema::SchemaSet& s = schema::Main();
    const char* name  = s.TableValue("ctdaFunctions", fn, "name");
    const char* kind1 = s.TableValue("ctdaFunctions", fn, "param1");
    const char* kind2 = s.TableValue("ctdaFunctions", fn, "param2");
    static const char* kOps[] = {"==", "!=", ">", ">=", "<", "<=", "?6", "?7"};
    std::printf("      CTDA fn=%u(%s) op %s ", fn, name ? name : "??", kOps[(op >> 5) & 7]);
    if (useGlobal) {
        std::printf("GLOB[%s]", p.FormIdToKey(cmp).ToString().c_str());
    } else {
        float f = 0;
        std::memcpy(&f, &cmp, 4);
        std::printf("%.3f", f);
    }
    auto pparam = [&](uint32_t v, const char* kind) {
        if (v == 0) { std::printf("0"); return; }
        if (kind && std::strcmp(kind, "form") == 0 && !aliases)
            std::printf("%s", p.FormIdToKey(v).ToString().c_str());
        else
            std::printf("%u", v);
    };
    std::printf("  p1="); pparam(p1, kind1);
    std::printf(" p2="); pparam(p2, kind2);
    std::printf("  runOn=%u", runOn);
    if (runOn == 2 && ref) std::printf("(ref %s)", p.FormIdToKey(ref).ToString().c_str());
    if (aliases) std::printf(" [params=aliasIdx]");
    if (isOr) std::printf(" [OR]");
    std::printf("\n");
}

static void DumpRecord(const Plugin& p, const RecordNode* node) {
    const auto pr = const_cast<RecordNode*>(node)->Payload();
    const FormKey key = p.FormIdToKey(node->header.formId);
    std::printf("== %s  %s  raw=0x%08X  flags=0x%08X%s%s  dataSize=%u\n",
                TagOf(node->header.sig).c_str(), key.ToString().c_str(),
                node->header.formId, node->header.flags,
                node->Compressed() ? " [zip]" : "",
                node->Deleted() ? " [del]" : "", node->header.dataSize);
    if (!pr) { std::printf("   (payload unavailable: %s)\n", pr.error().message.c_str()); return; }
    const std::span<const uint8_t> payload = *pr;

    SubIterator it(payload);
    SubView sub;
    while (it.Next(sub)) {
        const std::string tag = TagOf(sub.sig);

        if (sub.sig == Sig4("CTDA")) {
            std::printf("   CTDA (%u)\n", sub.size);
            DumpCtda(sub.data, sub.size, p);
            continue;
        }
        if (const char* fn = FillTypeName(sub.sig)) {
            std::printf("   %s (%u)  <fill:%s>", tag.c_str(), sub.size, fn);
            if (sub.size == 4) {
                uint32_t v = 0; std::memcpy(&v, sub.data, 4);
                std::printf("  %s", p.FormIdToKey(v).ToString().c_str());
            } else if (sub.size) { std::printf("  "); PrintHex(sub.data, sub.size); }
            std::printf("\n");
            continue;
        }

        std::printf("   %s (%u)  ", tag.c_str(), sub.size);
        if (sub.size == 4) {
            uint32_t v = 0; std::memcpy(&v, sub.data, 4);
            // Show both raw and FormKey — most 4-byte subs are FormIDs.
            std::printf("u32=0x%08X (%u)  key=%s", v, v,
                        p.FormIdToKey(v).ToString().c_str());
        } else if (sub.size == 2) {
            uint16_t v = 0; std::memcpy(&v, sub.data, 2);
            std::printf("u16=%u", v);
        } else if (sub.size == 1) {
            std::printf("u8=%u", sub.data[0]);
        } else if (LooksText(sub.data, sub.size)) {
            std::printf("\"%s\"", ReadZString(sub).c_str());
        } else {
            PrintHex(sub.data, sub.size);
        }
        std::printf("\n");
    }
    if (!it.ok()) std::printf("   (truncated subrecord stream)\n");
    std::printf("\n");
}

static std::string EdidOf(const Plugin& p, const RecordNode* node) {
    const auto payload = const_cast<RecordNode*>(node)->Payload();
    if (!payload) return {};
    SubIterator it(*payload);
    SubView sub;
    while (it.Next(sub))
        if (sub.sig == Sig4("EDID")) return ReadZString(sub);
    return {};
}

static void WalkGroups(const std::vector<Node>& nodes, int depth, int maxDepth) {
    for (const Node& n : nodes) {
        if (!n.IsGroup()) continue;
        const GrupHeader& g = n.group->header;
        size_t recs = 0, subgroups = 0;
        for (const Node& c : n.group->children)
            (c.IsGroup() ? subgroups : recs)++;
        std::string labelDesc;
        if (g.groupType == group_type::kTop)
            labelDesc = "sig=" + TagOf(g.label);
        else
            { char b[32]; std::snprintf(b, sizeof(b), "label=0x%08X", g.label); labelDesc = b; }
        std::printf("%*sGRUP type=%d %-14s  %zu rec, %zu subgrp\n",
                    depth * 2, "", g.groupType, labelDesc.c_str(), recs, subgroups);
        if (depth < maxDepth)
            WalkGroups(n.group->children, depth + 1, maxDepth);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage:\n"
                    "  esm-dump <plugin> grup [--depth N]\n"
                    "  esm-dump <plugin> sig SIG [--edid SUBSTR] [--limit N]\n"
                    "  esm-dump <plugin> form HEXID\n");
        return 2;
    }
    const std::string path = argv[1];
    const std::string cmd  = argv[2];

    Plugin p;
    if (auto loaded = p.Load(path); !loaded) {
        std::printf("FAIL load %s: %s\n", path.c_str(), loaded.error().message.c_str());
        return 1;
    }
    std::printf("# %s  %zu records  %zu masters%s\n\n", p.filename.c_str(),
                p.index.size(), p.masters.size(), p.isLight ? " [light]" : "");

    if (cmd == "grup") {
        int maxDepth = 2;
        for (int i = 3; i < argc - 1; ++i)
            if (std::strcmp(argv[i], "--depth") == 0) maxDepth = std::atoi(argv[i + 1]);
        WalkGroups(p.tree, 0, maxDepth);
        return 0;
    }

    if (cmd == "sig" && argc >= 4) {
        const uint32_t want = SigFromArg(argv[3]);
        std::string edid;
        int limit = 1000000;
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--edid") == 0 && i + 1 < argc) edid = argv[++i];
            else if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = std::atoi(argv[++i]);
        }
        std::string edidLower = edid;
        std::transform(edidLower.begin(), edidLower.end(), edidLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        // Deterministic order: sort matches by formId.
        std::vector<const RecordNode*> matches;
        for (const auto& [formId, node] : p.index) {
            (void)formId;
            if (node->header.sig != want) continue;
            if (!edidLower.empty()) {
                std::string e = EdidOf(p, node);
                std::transform(e.begin(), e.end(), e.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (e.find(edidLower) == std::string::npos) continue;
            }
            matches.push_back(node);
        }
        std::sort(matches.begin(), matches.end(),
                  [](const RecordNode* a, const RecordNode* b) {
                      return a->header.formId < b->header.formId;
                  });
        std::printf("# %zu match(es) for sig %s%s%s\n\n", matches.size(),
                    TagOf(want).c_str(), edid.empty() ? "" : " edid~",
                    edid.c_str());
        int n = 0;
        for (const RecordNode* node : matches) {
            if (n++ >= limit) break;
            DumpRecord(p, node);
        }
        return 0;
    }

    if (cmd == "form" && argc >= 4) {
        const uint32_t id = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16));
        auto it = p.index.find(id);
        if (it == p.index.end()) {
            std::printf("no record with on-disk formId 0x%08X\n", id);
            return 1;
        }
        DumpRecord(p, it->second);
        return 0;
    }

    std::printf("unknown command '%s'\n", cmd.c_str());
    return 2;
}
