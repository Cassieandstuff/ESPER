#define _CRT_SECURE_NO_WARNINGS

#include <ESPER/LoadOrderReader.h>

#include <ESPER/CaseUtil.h>
#include <ESPER/Plugin.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace esper {

namespace fs = std::filesystem;

static std::string Trim(const std::string& in) {
    size_t b = 0, e = in.size();
    if (e - b >= 3 &&
        static_cast<unsigned char>(in[b]) == 0xEF &&
        static_cast<unsigned char>(in[b + 1]) == 0xBB &&
        static_cast<unsigned char>(in[b + 2]) == 0xBF) {
        b += 3;
    }
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    return in.substr(b, e - b);
}

static bool FindInDir(const fs::path& dir, const std::string& name, std::string& outActual) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return false;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file(ec)) continue;
        const std::string fn = de.path().filename().string();
        if (EqualCI(fn, name)) { outActual = fn; return true; }
    }
    return false;
}

static bool Contains(const std::vector<PluginListing>& list, const std::string& name) {
    for (const auto& l : list)
        if (EqualCI(l.filename, name)) return true;
    return false;
}

static void AppendResolved(std::vector<PluginListing>& list, const fs::path& dataDir,
                           const std::string& requested, bool enabled) {
    if (Contains(list, requested)) return;
    std::string actual;
    const bool present = FindInDir(dataDir, requested, actual);
    PluginListing l;
    l.filename = present ? actual : requested;
    l.enabled  = enabled;
    l.missing  = !present;
    list.push_back(std::move(l));
}

Result<std::vector<PluginListing>> LoadOrderReader::ResolveOrder(
    const std::string& dataFolder, const std::string& pluginsTxtOverride) {
    std::vector<PluginListing> out;
    const fs::path dataDir(dataFolder);

    std::string pluginsTxt = pluginsTxtOverride;
    if (pluginsTxt.empty()) {
        const char* localApp = std::getenv("LOCALAPPDATA");
        if (!localApp || !*localApp)
            return Fail("LOCALAPPDATA not set and no plugins.txt override provided");
        pluginsTxt = (fs::path(localApp) / "Skyrim Special Edition" / "plugins.txt").string();
    }

    // 1. Implicit masters (only if actually present).
    static const char* kImplicit[] = {
        "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"
    };
    for (const char* name : kImplicit) {
        std::string actual;
        if (FindInDir(dataDir, name, actual))
            AppendResolved(out, dataDir, actual, true);
    }

    // 2. plugins.txt — BEFORE Skyrim.ccc, so an explicit user disable of a CC
    //    plugin wins over the ccc list (AppendResolved keeps first occurrence).
    {
        std::ifstream pf(fs::path(pluginsTxt), std::ios::binary);
        if (!pf) return Fail("cannot open plugins.txt: {}", pluginsTxt);
        std::string line;
        while (std::getline(pf, line)) {
            std::string s = Trim(line);
            if (s.empty() || s[0] == '#') continue;
            bool enabled = false;
            if (s[0] == '*') { enabled = true; s = Trim(s.substr(1)); }
            if (s.empty()) continue;
            AppendResolved(out, dataDir, s, enabled);
        }
    }

    // 3. Skyrim.ccc — Creation Club plugins not already listed load enabled.
    {
        fs::path cccPath;
        std::string cccActual;
        const fs::path parent = dataDir.parent_path();
        if (!parent.empty() && FindInDir(parent, "Skyrim.ccc", cccActual)) {
            cccPath = parent / cccActual;
        } else if (FindInDir(dataDir, "Skyrim.ccc", cccActual)) {
            cccPath = dataDir / cccActual;
        }
        if (!cccPath.empty()) {
            std::ifstream ccc(cccPath, std::ios::binary);
            if (ccc) {
                std::string line;
                while (std::getline(ccc, line)) {
                    const std::string name = Trim(line);
                    if (name.empty() || name[0] == '#') continue;
                    std::string actual;
                    if (FindInDir(dataDir, name, actual))
                        AppendResolved(out, dataDir, actual, true);
                }
            }
        }
    }

    // 4. True flags: TES4 header for present files, extension forcing on top
    //    (.esm → master; .esl → master + light). Missing files fall back to
    //    extension alone.
    for (PluginListing& l : out) {
        const std::string ext = ToLowerCI(fs::path(l.filename).extension().string());
        bool flagMaster = false, flagLight = false;
        if (!l.missing) {
            Plugin p;
            if (p.OpenHeader((dataDir / l.filename).string())) {
                flagMaster = p.isMaster;
                flagLight  = p.isLight;
            }
        }
        l.isMaster = flagMaster || ext == ".esm" || ext == ".esl";
        l.isLight  = flagLight  || ext == ".esl";
    }

    // 5. The engine's load rule: master-flagged plugins load before
    //    non-masters, stable within each class. (ESL-flagged .esp files —
    //    "ESPFE" — are NOT master-flagged and do not hoist.)
    std::stable_partition(out.begin(), out.end(),
                          [](const PluginListing& l) { return l.isMaster; });

    for (size_t i = 0; i < out.size(); ++i)
        out[i].loadIndex = static_cast<int>(i);

    return out;
}

} // namespace esper
