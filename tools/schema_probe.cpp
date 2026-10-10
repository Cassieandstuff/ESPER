// ---------------------------------------------------------------------------
// schema-probe — clean-room layout research on real data.
//
//   schema-probe <DataFolder> <REC> <SUB> [--size N] [--examples K] [--scan]
//
// Profiles every <SUB> subrecord inside <REC> records across the five vanilla
// masters: the payload-size histogram, then — for the most common size, or
// --size — one row per 4-byte-aligned column:
//   ref%   values that resolve, through the owning plugin's masters, to an
//          existing record (with the commonest target record types)
//   big%   of those, values outside the low engine range (local id >= 0x800 or
//          a non-zero master index) — small integers resolve by coincidence
//          because Skyrim.esm defines low ids; real data references are big
//   zero%  values that are 0
//   flt%   values that read as a plausible float (finite, 1e-6 <= |v| <= 1e7, or 0)
//   int%   values < 65536
//   uniq   distinct values
// plus the trailing bytes when the size isn't a multiple of 4, and a few hex
// examples. --scan also tests every byte offset (aligned or not) and lists
// those where non-zero u32 values mostly resolve as big references. A FormID column shows ref% near 100 (zero allowed for "none");
// data columns show ref% near 0. Evidence for descriptors — the schema gate
// then proves them.
// Opt-in target: configure with -DESPER_BUILD_TOOLS=ON.
// ---------------------------------------------------------------------------
#include <ESPER/LoadOrderIndex.h>
#include <ESPER/Plugin.h>
#include <ESPER/PluginFlags.h>
#include <ESPER/Subrecords.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace esper;

namespace {

uint32_t SigFrom(const char* s) {
    uint32_t v = 0;
    std::memcpy(&v, s, std::min<size_t>(4, std::strlen(s)));
    return v;
}

std::string SigText(uint32_t sig) {
    std::string s(4, ' ');
    std::memcpy(s.data(), &sig, 4);
    return s;
}

struct Column {
    size_t n = 0, ref = 0, big = 0, zero = 0, flt = 0, small = 0;
    std::map<uint32_t, size_t> targets;
    std::set<uint32_t> distinct;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: schema-probe <DataFolder> <REC> <SUB> [--size N] [--examples K]\n");
        return 2;
    }
    const fs::path data(argv[1]);
    const uint32_t recSig = SigFrom(argv[2]);
    const uint32_t subSig = SigFrom(argv[3]);
    long wantSize = -1;
    int examples = 4;
    bool scan = false, brief = false;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scan") scan = true;
        else if (a == "--brief") { brief = true; examples = 0; }
        else if (a == "--size" && i + 1 < argc) wantSize = std::stol(argv[++i]);
        else if (a == "--examples" && i + 1 < argc) examples = std::stoi(argv[++i]);
    }

    std::vector<std::unique_ptr<Plugin>> plugins;
    for (const char* n : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" }) {
        auto p = std::make_unique<Plugin>();
        if (!p->Load((data / n).string())) continue;
        plugins.push_back(std::move(p));
    }
    LoadOrderIndex index;
    index.Build(std::move(plugins));

    // Pass 1: sizes.
    std::map<uint32_t, size_t> sizes;
    size_t recs = 0;
    for (const auto& p : index.Plugins())
        for (const auto& [raw, node] : p->index) {
            if (node->header.sig != recSig) continue;
            SubIterator it(node->Payload().value_or(std::span<const uint8_t>{}));
            SubView s;
            bool any = false;
            while (it.Next(s))
                if (s.sig == subSig) { ++sizes[s.size]; any = true; }
            recs += any;
        }
    if (!brief)
        std::printf("%s %s: %zu record(s) carry it\n", SigText(recSig).c_str(), SigText(subSig).c_str(), recs);
    std::vector<std::pair<size_t, uint32_t>> bySize;
    for (const auto& [s, c] : sizes) bySize.push_back({ c, s });
    std::sort(bySize.rbegin(), bySize.rend());
    std::printf(brief ? "  sizes:" : "sizes:");
    for (size_t i = 0; i < bySize.size() && i < 12; ++i)
        std::printf(" %u x%zu", bySize[i].second, bySize[i].first);
    std::printf("%s\n", bySize.size() > 12 ? " ..." : "");
    if (bySize.empty()) return 0;
    const uint32_t size = wantSize >= 0 ? static_cast<uint32_t>(wantSize) : bySize[0].second;

    // Pass 2: columns of the chosen size.
    std::vector<Column> cols(size / 4);
    std::map<uint32_t, size_t> tailValues;
    int shown = 0;
    for (const auto& p : index.Plugins())
        for (const auto& [raw, node] : p->index) {
            if (node->header.sig != recSig) continue;
            SubIterator it(node->Payload().value_or(std::span<const uint8_t>{}));
            SubView s;
            while (it.Next(s)) {
                if (s.sig != subSig || s.size != size) continue;
                if (shown < examples) {
                    std::printf("  e.g. %s %08X:", p->filename.c_str(), raw);
                    for (uint32_t b = 0; b < s.size && b < 64; ++b)
                        std::printf("%s%02X", (b % 4 == 0) ? " " : "", s.data[b]);
                    std::printf("%s\n", s.size > 64 ? " ..." : "");
                    ++shown;
                }
                for (size_t c = 0; c < cols.size(); ++c) {
                    uint32_t v = 0;
                    std::memcpy(&v, s.data + c * 4, 4);
                    Column& col = cols[c];
                    ++col.n;
                    if (v == 0) ++col.zero;
                    if (v < 65536) ++col.small;
                    const float fl = std::bit_cast<float>(v);
                    if (v == 0 || (std::isfinite(fl) && std::fabs(fl) >= 1e-6f && std::fabs(fl) <= 1e7f)) ++col.flt;
                    if (col.distinct.size() < 100000) col.distinct.insert(v);
                    if (v != 0) {
                        if (const ResolvedRecord* r = index.ResolveRef(p.get(), v, 0)) {
                            ++col.ref;
                            if ((v & 0x00FFFFFFu) >= 0x800 || (v >> 24) != 0) ++col.big;
                            ++col.targets[r->Sig()];
                        }
                    }
                }
                if (size % 4) {
                    uint32_t tail = 0;
                    std::memcpy(&tail, s.data + (size / 4) * 4, size % 4);
                    ++tailValues[tail];
                }
            }
        }
    if (brief) {
        // One line: each aligned column classified as F(formid: >= 95% of its
        // non-zero values are big references; targets), f (float), i (int) or
        // z (always zero), then any unaligned offset that looks like a reference.
        std::printf("%s %s [%u]:", SigText(recSig).c_str(), SigText(subSig).c_str(), size);
        for (size_t c = 0; c < cols.size(); ++c) {
            const Column& col = cols[c];
            const size_t nonzero = col.n - col.zero;
            std::string cls;
            if (nonzero == 0) {
                cls = "z";
            } else if (static_cast<double>(col.big) >= 0.95 * static_cast<double>(nonzero)) {
                cls = "F(";
                std::vector<std::pair<size_t, uint32_t>> t;
                for (const auto& [sig, k] : col.targets) t.push_back({ k, sig });
                std::sort(t.rbegin(), t.rend());
                for (size_t i = 0; i < t.size() && i < 4; ++i) cls += (i ? "," : "") + SigText(t[i].second);
                cls += ")";
            } else if (col.flt >= col.n * 99 / 100 && col.small < col.n / 2) {
                cls = "f";
            } else {
                cls = "i";
            }
            std::printf(" %zu:%s", c * 4, cls.c_str());
        }
        if (size % 4) std::printf(" +%ub", size % 4);
        std::printf("\n");
        scan = true;
    } else {
        std::printf("size %u: %zu column(s)\n  off   ref%%  big%%  zero%%  flt%%  int%%   uniq  targets\n", size, cols.size());
    }
    for (size_t c = 0; c < cols.size() && !brief; ++c) {
        const Column& col = cols[c];
        if (!col.n) continue;
        auto pct = [&](size_t k) { return 100.0 * static_cast<double>(k) / static_cast<double>(col.n); };
        std::vector<std::pair<size_t, uint32_t>> t;
        for (const auto& [sig, k] : col.targets) t.push_back({ k, sig });
        std::sort(t.rbegin(), t.rend());
        std::string tg;
        for (size_t i = 0; i < t.size() && i < 4; ++i) tg += SigText(t[i].second) + ":" + std::to_string(t[i].first) + " ";
        std::printf("  %3zu  %5.1f %5.1f  %5.1f  %5.1f  %5.1f  %6zu%s  %s\n", c * 4, pct(col.ref), pct(col.big), pct(col.zero),
                    pct(col.flt), pct(col.small), col.distinct.size(), col.distinct.size() >= 100000 ? "+" : " ",
                    tg.c_str());
    }
    if (scan && size >= 4) {
        // Every byte offset, aligned or not: where do u32 values look like
        // real references? Evidence for (or against) a "no FormIDs" claim.
        std::vector<size_t> hits(size - 3, 0), seen(size - 3, 0);
        for (const auto& p : index.Plugins())
            for (const auto& [raw, node] : p->index) {
                if (node->header.sig != recSig) continue;
                SubIterator it(node->Payload().value_or(std::span<const uint8_t>{}));
                SubView s;
                while (it.Next(s)) {
                    if (s.sig != subSig || s.size != size) continue;
                    for (uint32_t o = 0; o + 4 <= size; ++o) {
                        uint32_t v = 0;
                        std::memcpy(&v, s.data + o, 4);
                        if (v == 0) continue;
                        ++seen[o];
                        if (((v & 0x00FFFFFFu) >= 0x800 || (v >> 24) != 0) &&
                            index.ResolveRef(p.get(), v, 0))
                            ++hits[o];
                    }
                }
            }
        std::printf("  scan: byte offsets where >= 20%% of non-zero u32 values are big references:");
        bool any = false;
        for (uint32_t o = 0; o + 4 <= size; ++o) {
            if (!seen[o]) continue;
            const double pct = 100.0 * static_cast<double>(hits[o]) / static_cast<double>(seen[o]);
            if (pct >= 20.0) { std::printf(" %u(%.0f%%)", o, pct); any = true; }
        }
        std::printf("%s\n", any ? "" : " none");
    }
    if (size % 4) {
        std::printf("  tail (%u byte(s)) values:", size % 4);
        size_t k = 0;
        for (const auto& [v, c] : tailValues) {
            if (k++ >= 10) { std::printf(" ..."); break; }
            std::printf(" %X x%zu", v, c);
        }
        std::printf("\n");
    }
    return 0;
}
