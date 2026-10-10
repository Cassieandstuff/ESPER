// ---------------------------------------------------------------------------
// esm-dump — read-only research dumper on the ESPER read path.
//
// Purpose: ground-truth inspection of shipped game data — Story Manager node
// linkage (SMBN/SMQN/SMEN + PNAM parent graph), quest alias/package sections,
// and CTDA condition rows (decoded via the clean-room function table). Answers
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

static void DumpCtda(const uint8_t* d, uint32_t n, const Plugin& p) {
    if (n < sizeof(CondData)) { std::printf("      (short CTDA %u)\n", n); return; }
    CondData c{};
    std::memcpy(&c, d, sizeof(CondData));
    CtdaFuncInfo info;
    const bool known = LookupCtdaFunc(c.functionIndex, info);
    static const char* kOps[] = {"==", "!=", ">", ">=", "<", "<=", "?6", "?7"};
    std::printf("      CTDA fn=%u(%s) op %s ", c.functionIndex,
                known ? info.name : "??", kOps[c.CompareOp() & 7]);
    if (c.UsesGlobal())
        std::printf("GLOB[%s]", p.FormIdToKey(c.comparisonRaw).ToString().c_str());
    else
        std::printf("%.3f", c.ComparisonValue());
    // Parameters, classified by the function table when known.
    auto pparam = [&](uint32_t v, CtdaParam kind) {
        if (v == 0) { std::printf("0"); return; }
        if (kind == CtdaParam::Form && !c.ParamsUseAliases())
            std::printf("%s", p.FormIdToKey(v).ToString().c_str());
        else
            std::printf("%u", v);
    };
    std::printf("  p1="); pparam(c.param1, known ? info.p1 : CtdaParam::Int);
    std::printf(" p2="); pparam(c.param2, known ? info.p2 : CtdaParam::Int);
    std::printf("  runOn=%u", c.runOnType);
    if (c.runOnType == CondData::kRunOnReference && c.reference)
        std::printf("(ref %s)", p.FormIdToKey(c.reference).ToString().c_str());
    if (c.ParamsUseAliases()) std::printf(" [params=aliasIdx]");
    if (c.IsOr()) std::printf(" [OR]");
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
