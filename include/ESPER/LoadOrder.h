#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// esper::loadorder — the optional load-order layer. Link `esper::loadorder` and
// include this header (it includes <ESPER.h>).
//
// LoadOrderDocument resolves a Skyrim SE/AE load order (implicit masters,
// plugins.txt, Skyrim.ccc, the master hoist), loads every enabled plugin, and
// answers questions about the game's data as the game sees it: override
// winners and chains, typed decoding through the schema, localized names,
// EditorID/name search, worldspaces and cells, and copying a record into a
// plugin being authored.
//
//   esper::LoadOrderDocument doc;
//   esper::LoadOrderOptions opt;
//   opt.dataFolder = R"(C:\Steam\steamapps\common\Skyrim Special Edition\Data)";
//   doc.Load(opt);                                   // plugins.txt from %LOCALAPPDATA%
//   for (auto* r : doc.Search(esper::Sig4("NPC_"), "Lydia")) {
//       auto d = doc.Decode(*r);
//       esper::view::Npc npc(d->record);
//       ...
//   }
//
// Loading costs a header scan per plugin: record payloads are views into the
// file buffers, inflated and decoded only when asked for. Not thread-safe —
// decoding inflates compressed records and loads string tables lazily.
// ─────────────────────────────────────────────────────────────────────────────

#include <ESPER.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace esper {

enum class ResolveTarget {
    Winner,   // the highest-priority plugin's version (what the game uses)
    Origin,   // the version in the plugin that defines the record
};

struct LoadOrderOptions {
    // The game's Data folder; plugins load from here.
    std::string dataFolder;
    // plugins.txt to read. Empty: %LOCALAPPDATA%\Skyrim Special Edition\plugins.txt.
    // Pass a mod manager's profile file to read its load order instead.
    std::string pluginsTxt;
    // Localized-strings language, as in Strings/<Plugin>_<language>.STRINGS.
    std::string language = "English";
    // Resolves a Data-relative path (e.g. "Strings/Skyrim_English.STRINGS") to
    // bytes. Default: loose files under dataFolder. Supply one that also reads
    // BSAs (vanilla string tables ship in archives) to resolve vanilla names.
    StringsSet::FetchFn fetch;
};

// A record decoded from the load order. Its FormIDs are in the on-disk space
// of `source->owner`, the plugin it came from; follow them with Follow().
struct DecodedRecord {
    const ResolvedRecord* source = nullptr;
    Record                record;
};

class LoadOrderDocument {
public:
    // Resolves and loads the load order. A plugin that fails to load is
    // skipped and reported in Warnings(); a load order that can't be resolved
    // at all (no plugins.txt, no Data folder) fails.
    bool Load(const LoadOrderOptions& options, std::string* err = nullptr);
    bool IsLoaded() const { return loaded_; }

    const LoadOrderOptions&                     Options() const { return options_; }
    // The resolved load order, including disabled and missing entries.
    const std::vector<PluginListing>&           Listing() const { return listing_; }
    // The loaded plugins, in load order.
    const std::vector<std::unique_ptr<Plugin>>& Plugins() const { return index_.Plugins(); }
    const std::vector<std::string>&             Warnings() const { return warnings_; }
    const LoadOrderIndex&                       Index() const { return index_; }
    const Plugin*                               FindPlugin(std::string_view filename) const;

    // ── Records ──────────────────────────────────────────────────────────────
    // nullptr when unknown, or (Winner) when the winning version is deleted.
    const ResolvedRecord* Resolve(const FormKey& key,
                                  ResolveTarget target = ResolveTarget::Winner) const;
    // Every version, oldest (origin) to newest (winner).
    const std::vector<ResolvedRecord>* Chain(const FormKey& key) const;
    // Winning, non-deleted records of `sig`, in deterministic order.
    std::vector<const ResolvedRecord*> Enumerate(uint32_t sig) const;
    // Follows a FormID found inside `from`'s record.
    const ResolvedRecord* Follow(const ResolvedRecord& from, FormId id,
                                 ResolveTarget target = ResolveTarget::Winner) const;
    // Follows a typed link; nullptr if it resolves to a different record type.
    template <class View>
    const ResolvedRecord* Follow(const ResolvedRecord& from, Link<View> link,
                                 ResolveTarget target = ResolveTarget::Winner) const {
        const ResolvedRecord* r = Follow(from, link.id, target);
        return (r && r->Sig() == View::kSig) ? r : nullptr;
    }

    // Decodes with the schema engine (schema::Main()).
    std::optional<DecodedRecord> Decode(const ResolvedRecord& r, std::string* err = nullptr) const;
    std::optional<DecodedRecord> Decode(const FormKey& key,
                                        ResolveTarget target = ResolveTarget::Winner,
                                        std::string* err = nullptr) const;

    // ── Names ────────────────────────────────────────────────────────────────
    // The text of an lstring value from a record of `owner` (its inline text,
    // or a string-table lookup when `owner` is localized). "" when missing.
    std::string Text(const Plugin& owner, const LocalizedString& s, schema::StrTable table) const;
    std::string EditorId(const ResolvedRecord& r) const;
    std::string Name(const ResolvedRecord& r) const;     // FULL, resolved; "" if none
    // Winning records of `sig` whose EditorID or name contains `text`
    // (ASCII case-insensitive). Reads EDID/FULL only — no full decode.
    std::vector<const ResolvedRecord*> Search(uint32_t sig, std::string_view text,
                                              size_t maxResults = SIZE_MAX) const;

    // ── Worldspaces and cells ────────────────────────────────────────────────
    // Built on first use.
    const WorldIndex& World() const;

    // ── Authoring ────────────────────────────────────────────────────────────
    // Copies the winning version of `key` into `target` (a plugin being
    // authored; not one owned by this document) as an override. Adds masters
    // as needed, rewrites every FormID into `target`'s space, and converts
    // localized strings to inline text when `target` isn't localized. Returns
    // the existing node if `target` already overrides the record.
    //
    // Refuses records that live in cell, worldspace or topic children
    // (REFR/ACHR/LAND/NAVM/..., CELL, INFO), and records whose FormIDs or
    // strings aren't all visible to the schema (Record::RemapSafe).
    RecordNode* OverrideInto(Plugin& target, const FormKey& key, std::string* err = nullptr) const;

    // ── Snapshot ─────────────────────────────────────────────────────────────
    // The resolved load order as JSON (folders, language, and each plugin's
    // load index, flags, enabled/missing state and masters), for tools that
    // don't link ESPER.
    std::string SnapshotJson() const;

private:
    StringsSet* StringsFor(const Plugin& owner) const;
    bool        HeaderStrings(const ResolvedRecord& r, std::string* edid, std::string* name) const;

    LoadOrderOptions           options_;
    std::vector<PluginListing> listing_;
    std::vector<std::string>   warnings_;
    LoadOrderIndex             index_;
    bool                       loaded_ = false;

    mutable std::unordered_map<const Plugin*, std::unique_ptr<StringsSet>> strings_;
    mutable std::unique_ptr<WorldIndex>                                    world_;
};

} // namespace esper
