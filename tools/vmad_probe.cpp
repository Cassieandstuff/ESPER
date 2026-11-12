// ---------------------------------------------------------------------------
// vmad-probe — clean-room layout research on VMAD's per-record trailer.
//
//   vmad-probe <DataFolder> <REC> [--examples K] [--edid SUBSTR] [--min N]
//
// Decodes every <REC> record's VMAD across the five vanilla masters with the
// schema engine, and profiles the bytes after the scripts array (the trailer
// the schema keeps opaque): how many records carry one, its size histogram,
// and K examples in hex beside the record's EditorID, its script names and
// the counts of the record's own sections (QUST: reference / location
// aliases, stages). Evidence for the trailer's descriptor; the gate proves it.
// Read-only. Opt-in target: configure with -DESPER_BUILD_TOOLS=ON.
// ---------------------------------------------------------------------------
#include <ESPER.h>
#include <ESPER/LoadOrderIndex.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
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

void Dump(const std::vector<uint8_t>& b, size_t cap) {
    for (size_t row = 0; row < b.size() && row < cap; row += 32) {
        std::printf("      %04zx ", row);
        for (size_t i = row; i < row + 32; ++i)
            if (i < b.size()) std::printf("%02X%s", b[i], (i % 4 == 3) ? " " : "");
            else std::printf("  %s", (i % 4 == 3) ? " " : "");
        std::printf(" |");
        for (size_t i = row; i < row + 32 && i < b.size(); ++i) std::printf("%c", std::isprint(b[i]) ? b[i] : '.');
        std::printf("|\n");
    }
    if (b.size() > cap) std::printf("      ... (%zu bytes)\n", b.size());
}

/// The VMAD value's script list and its still-opaque trailer: vmad = [version, objFormat, body(union) -> (union) ->
/// {scripts, fragments (a union by record: a modeled layout, or `other` bytes)}]. `typed` is set when the record's trailer
/// is modeled (it then has no opaque bytes to show).
bool Split(const Value& vmad, const Value*& scripts, const Value*& trailer, bool& typed) {
    if (vmad.items.size() < 3) return false;
    const Value& body = vmad.items[2];
    if (body.items.empty()) return false;
    const Value& inner = body.items[0];
    if (inner.items.empty()) return false;
    const Value& s = inner.items[0];
    if (s.items.size() < 2) return false;
    scripts = &s.items[0];
    trailer = &s.items[1];
    typed   = false;
    if (!trailer->items.empty()) {                     // the union's payload
        const Value& payload = trailer->items[0];
        typed   = payload.bytes.empty() && !payload.items.empty();
        trailer = &payload;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: vmad-probe <DataFolder> <REC> [--examples K] [--edid SUBSTR] [--min N]\n");
        return 2;
    }
    const fs::path data(argv[1]);
    const uint32_t recSig = SigFrom(argv[2]);
    int            examples = 6;
    size_t         minSize  = 1;
    std::string    edidFilter;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--examples" && i + 1 < argc) examples = std::stoi(argv[++i]);
        else if (a == "--edid" && i + 1 < argc) edidFilter = argv[++i];
        else if (a == "--min" && i + 1 < argc) minSize = static_cast<size_t>(std::stoul(argv[++i]));
    }
    std::transform(edidFilter.begin(), edidFilter.end(), edidFilter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<std::unique_ptr<Plugin>> plugins;
    for (const char* n : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" }) {
        auto p = std::make_unique<Plugin>();
        if (!p->Load((data / n).string())) continue;
        plugins.push_back(std::move(p));
    }
    LoadOrderIndex index;
    index.Build(std::move(plugins));

    size_t                     records = 0, withVmad = 0, undecoded = 0, withTrailer = 0, modeled = 0;
    std::map<size_t, size_t>   sizes;
    std::map<std::string, size_t> firstBytes;
    int                        shown = 0;
    for (const auto& p : index.Plugins())
        for (const auto& [raw, node] : p->index) {
            if (node->header.sig != recSig) continue;
            ++records;
            auto rec = Record::Decode(*p, *node);
            if (!rec) { ++undecoded; continue; }
            const auto path = rec->Find("vmad");
            if (!path) continue;
            const Value* vmad = rec->GetValue(*path);
            if (!vmad) {
                // present but raw (refused)?
                bool raw = false;
                for (uint32_t s : rec->RawSigs()) raw = raw || s == SigFrom("VMAD");
                if (raw) ++undecoded;
                continue;
            }
            ++withVmad;
            const Value* scripts = nullptr;
            const Value* trailer = nullptr;
            bool typed = false;
            if (!Split(*vmad, scripts, trailer, typed)) continue;
            if (typed) { ++modeled; continue; }
            const std::vector<uint8_t>& t = trailer->bytes;
            if (t.empty()) continue;
            ++withTrailer;
            ++sizes[t.size()];
            char head[8];
            std::snprintf(head, sizeof head, "%02X", t[0]);
            ++firstBytes[head];
            if (t.size() < minSize) continue;

            std::string edid;
            if (auto e = rec->Find("editorId")) edid = rec->Root().Get<std::string>(*e).value_or("");
            std::string low = edid;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!edidFilter.empty() && low.find(edidFilter) == std::string::npos) continue;
            if (shown >= examples) continue;
            ++shown;
            std::printf("  %s %08X %s  version=%llu objFormat=%llu scripts=%zu",
                        p->filename.c_str(), raw, edid.c_str(), static_cast<unsigned long long>(vmad->items[0].bits),
                        static_cast<unsigned long long>(vmad->items[1].bits), scripts->items.size());
            for (const Value& s : scripts->items)
                if (!s.items.empty()) std::printf(" [%s]", s.items[0].str.c_str());
            for (const char* section : { "referenceAliases", "locationAliases", "stages" })
                if (auto sp = rec->Find(section)) std::printf(" %s=%zu", section, rec->Root().Groups(*sp).size());
            std::printf("  trailer %zu bytes\n", t.size());
            Dump(t, 512);
        }

    std::printf("%s: %zu record(s), %zu with a decoded VMAD, %zu undecoded/raw VMAD, %zu with a modeled trailer, %zu with an opaque trailer\n",
                SigText(recSig).c_str(),
                records, withVmad, undecoded, modeled, withTrailer);
    std::printf("trailer sizes:");
    std::vector<std::pair<size_t, size_t>> bySize;
    for (const auto& [s, c] : sizes) bySize.push_back({ c, s });
    std::sort(bySize.rbegin(), bySize.rend());
    for (size_t i = 0; i < bySize.size() && i < 20; ++i) std::printf(" %zu x%zu", bySize[i].second, bySize[i].first);
    std::printf("%s\nfirst byte:", bySize.size() > 20 ? " ..." : "");
    for (const auto& [b, c] : firstBytes) std::printf(" %s x%zu", b.c_str(), c);
    std::printf("\n");
    return 0;
}
