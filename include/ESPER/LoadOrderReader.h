#pragma once
#include <ESPER/Error.h>
#include <cstdint>
#include <string>
#include <vector>

namespace esper {

// One entry in a resolved load order.
struct PluginListing {
    std::string filename;
    int  loadIndex = 0;
    bool isMaster  = false;   // TES4 kMaster flag OR .esm/.esl extension
    bool isLight   = false;   // TES4 kLight flag OR .esl extension
    bool enabled   = true;
    bool missing   = false;
};

class LoadOrderReader {
public:
    // Resolves the final ordered plugin list for Skyrim SE/AE:
    //   1. Implicit masters (Skyrim.esm, Update, DG, HF, DB)
    //   2. plugins.txt entries (leading '*' = enabled)
    //   3. Creation Club entries from Skyrim.ccc that plugins.txt does not
    //      mention (so an explicit plugins.txt disable wins), enabled.
    // Flags come from each present file's TES4 header (extension forces:
    // .esm → master, .esl → master+light), and master-flagged entries are
    // hoisted above non-masters (stable within each class) — the engine's
    // load rule, which the override chains depend on.
    // Fails only on hard failures (e.g. plugins.txt missing).
    static Result<std::vector<PluginListing>> ResolveOrder(const std::string& dataFolder,
                                                           const std::string& pluginsTxtOverride);
};

} // namespace esper
