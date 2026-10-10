#include <ESPER/LoadOrder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace esper {

namespace fs = std::filesystem;

namespace {

// Records that live inside cell / worldspace / topic children groups, not in
// a top group: OverrideInto can't place them with InsertRecord.
bool LivesInChildren(uint32_t sig) {
    static const uint32_t kSigs[] = {
        Sig4("CELL"), Sig4("REFR"), Sig4("ACHR"), Sig4("PGRE"), Sig4("PHZD"), Sig4("PMIS"),
        Sig4("PARW"), Sig4("PBAR"), Sig4("PBEA"), Sig4("PCON"), Sig4("PFLA"), Sig4("LAND"),
        Sig4("NAVM"), Sig4("INFO"),
    };
    return std::find(std::begin(kSigs), std::end(kSigs), sig) != std::end(kSigs);
}

std::string JsonString(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

} // namespace

// ── Loading ──────────────────────────────────────────────────────────────────

Result<void> LoadOrderDocument::Load(const LoadOrderOptions& options) {
    *this = LoadOrderDocument{};
    options_ = options;
    if (!options_.fetch) {
        const std::string root = options_.dataFolder;
        options_.fetch = [root](const std::string& rel, std::vector<uint8_t>& out) {
            std::ifstream f(fs::path(root) / rel, std::ios::binary);
            if (!f) return false;
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return true;
        };
    }

    auto listing = LoadOrderReader::ResolveOrder(options_.dataFolder, options_.pluginsTxt);
    if (!listing) return std::unexpected(listing.error());
    listing_ = std::move(*listing);

    std::vector<std::unique_ptr<Plugin>> plugins;
    for (const PluginListing& l : listing_) {
        if (!l.enabled || l.missing) continue;
        auto p = std::make_unique<Plugin>();
        if (auto r = p->Load((fs::path(options_.dataFolder) / l.filename).string()); !r) {
            warnings_.push_back(l.filename + ": " + r.error().message);
            continue;
        }
        p->filename = l.filename;   // the load order's spelling
        for (const Diagnostic& d : p->diagnostics)
            warnings_.push_back(l.filename + ": " + d.message);
        plugins.push_back(std::move(p));
    }
    index_.Build(std::move(plugins));
    loaded_ = true;
    return {};
}

const Plugin* LoadOrderDocument::FindPlugin(std::string_view filename) const {
    const std::string name(filename);
    for (const auto& p : index_.Plugins())
        if (EqualCI(p->filename, name)) return p.get();
    return nullptr;
}

// ── Records ──────────────────────────────────────────────────────────────────

const ResolvedRecord* LoadOrderDocument::Resolve(const FormKey& key, ResolveTarget target) const {
    if (target == ResolveTarget::Winner) return index_.Resolve(key);
    const std::vector<ResolvedRecord>* chain = index_.ResolveChain(key);
    return (chain && !chain->empty()) ? &chain->front() : nullptr;
}

const std::vector<ResolvedRecord>* LoadOrderDocument::Chain(const FormKey& key) const {
    return index_.ResolveChain(key);
}

std::vector<const ResolvedRecord*> LoadOrderDocument::Enumerate(uint32_t sig) const {
    std::vector<const ResolvedRecord*> out;
    index_.EnumerateWinning(sig, out);
    return out;
}

const ResolvedRecord* LoadOrderDocument::Follow(const ResolvedRecord& from, FormId id,
                                                ResolveTarget target) const {
    if (id.raw == 0 || !from.owner) return nullptr;
    return Resolve(from.owner->FormIdToKey(id.raw), target);
}

Result<DecodedRecord> LoadOrderDocument::Decode(const ResolvedRecord& r) const {
    if (!r.owner || !r.node) return Fail("Decode: empty ResolvedRecord");
    auto rec = Record::Decode(*r.owner, *r.node);
    if (!rec) return Wrap(rec.error(), "{} in {}", r.key.ToString(), r.owner->filename);
    return DecodedRecord{ &r, std::move(*rec) };
}

Result<DecodedRecord> LoadOrderDocument::Decode(const FormKey& key, ResolveTarget target) const {
    const ResolvedRecord* r = Resolve(key, target);
    if (!r) return Fail("no record {}", key.ToString());
    return Decode(*r);
}

// ── Names ────────────────────────────────────────────────────────────────────

StringsSet* LoadOrderDocument::StringsFor(const Plugin& owner) const {
    auto it = strings_.find(&owner);
    if (it == strings_.end()) {
        auto set = std::make_unique<StringsSet>();
        set->Init(owner.filename, options_.language, options_.fetch);
        it = strings_.emplace(&owner, std::move(set)).first;
    }
    return it->second.get();
}

std::string LoadOrderDocument::Text(const Plugin& owner, const LocalizedString& s) const {
    if (!owner.localized) return s.text;
    std::string out;
    if (s.id == 0 || !StringsFor(owner)->Find(s.table, s.id, out)) return {};
    return out;
}

std::string LoadOrderDocument::Text(const DecodedRecord& d, const LocalizedString& s) const {
    if (!d.source || !d.source->owner) return s.text;
    return Text(*d.source->owner, s);
}

bool LoadOrderDocument::HeaderStrings(const ResolvedRecord& r, std::string* edid,
                                      std::string* name) const {
    const auto payload = r.node->Payload();
    if (!payload) return false;
    SubIterator it(*payload);
    SubView s;
    bool haveEdid = !edid, haveName = !name;
    while ((!haveEdid || !haveName) && it.Next(s)) {
        if (!haveEdid && s.sig == Sig4("EDID")) {
            *edid = ReadZString(s);
            haveEdid = true;
        } else if (!haveName && s.sig == Sig4("FULL")) {
            LocalizedString ls;
            if (r.owner->localized) {
                if (s.size == 4) std::memcpy(&ls.id, s.data, 4);
            } else {
                ls.text = ReadZString(s);
            }
            ls.table = schema::StrTable::Strings;
            *name = Text(*r.owner, ls);
            haveName = true;
        }
    }
    return true;
}

std::string LoadOrderDocument::EditorId(const ResolvedRecord& r) const {
    std::string edid;
    HeaderStrings(r, &edid, nullptr);
    return edid;
}

std::string LoadOrderDocument::Name(const ResolvedRecord& r) const {
    std::string name;
    HeaderStrings(r, nullptr, &name);
    return name;
}

std::vector<const ResolvedRecord*> LoadOrderDocument::Search(uint32_t sig, std::string_view text,
                                                             size_t maxResults) const {
    std::vector<const ResolvedRecord*> out;
    const std::string needle(text);
    for (const ResolvedRecord* r : Enumerate(sig)) {
        if (out.size() >= maxResults) break;
        std::string edid, name;
        if (!HeaderStrings(*r, &edid, &name)) continue;
        if (needle.empty() || ContainsCI(edid, needle) || ContainsCI(name, needle))
            out.push_back(r);
    }
    return out;
}

// ── Worldspaces ──────────────────────────────────────────────────────────────

const WorldIndex& LoadOrderDocument::World() const {
    if (!world_) {
        world_ = std::make_unique<WorldIndex>();
        world_->Build(index_);
    }
    return *world_;
}

// ── Authoring ────────────────────────────────────────────────────────────────

Result<RecordNode*> LoadOrderDocument::OverrideInto(Plugin& target, const FormKey& key) const {
    auto fail = [&](const std::string& why) -> std::unexpected<Error> {
        return Fail("OverrideInto {}: {}", key.ToString(), why);
    };
    const ResolvedRecord* w = Resolve(key, ResolveTarget::Winner);
    if (!w) return fail("no winning record in the load order");
    if (w->owner == &target) return fail("the target plugin is part of this load order");
    if (LivesInChildren(w->Sig()))
        return fail("records in cell, worldspace or topic children can't be placed in a top group");

    // Already overridden in `target`? (Checked without adding a master.)
    if (EqualCI(key.plugin, target.filename)) return fail("the record is defined by the target plugin");
    for (size_t m = 0; m < target.masters.size(); ++m) {
        if (!EqualCI(target.masters[m], key.plugin)) continue;
        const uint32_t id = (static_cast<uint32_t>(m) << 24) | (key.localId & 0x00FFFFFFu);
        if (auto it = target.index.find(id); it != target.index.end()) return it->second;
    }

    auto d = Decode(*w);
    if (!d) return fail(d.error().message);
    Record& rec = d->record;
    if (auto safe = rec.CheckRemapSafe(); !safe)
        return fail("its FormIDs aren't all visible to the schema: " + safe.error().message);
    if (rec.Context().localized != target.localized) {
        const Plugin& owner = *w->owner;
        const auto lookup = [&](uint32_t id, schema::StrTable table) -> std::optional<std::string> {
            std::string text;
            if (!StringsFor(owner)->Find(table, id, text)) return std::nullopt;
            return text;
        };
        if (auto r = rec.Relocalize(target.localized, lookup); !r) return fail(r.error().message);
    }

    // Every plugin a FormID points into becomes a master, in load order, before
    // any FormID is rewritten (adding a master renumbers `target`'s own records).
    std::set<size_t> needed;
    const auto& plugins = index_.Plugins();
    auto loadIndexOf = [&](const std::string& name) -> size_t {
        for (size_t i = 0; i < plugins.size(); ++i)
            if (EqualCI(plugins[i]->filename, name)) return i;
        return plugins.size();
    };
    needed.insert(loadIndexOf(key.plugin));          // the overridden record's own plugin
    rec.ForEachFormId([&](FormId id, std::span<const uint32_t>) {
        if (id.raw != 0) needed.insert(loadIndexOf(w->owner->FormIdToKey(id.raw).plugin));
    });
    for (size_t i : needed) {
        if (i >= plugins.size()) return fail("a FormID points into a plugin outside the load order");
        if (EqualCI(plugins[i]->filename, target.filename)) continue;
        if (auto r = EnsureMaster(target, plugins[i]->filename); !r) return fail(r.error().message);
    }

    std::optional<Error> refError;
    const auto visited = rec.VisitFormIds([&](FormId& id) {
        if (id.raw == 0 || refError) return;
        const auto mapped = MakeRef(target, w->owner->FormIdToKey(id.raw));
        if (!mapped) refError = mapped.error();
        else id.raw = *mapped;
    });
    if (!visited) return fail(visited.error().message);
    if (refError) return fail(refError->message);

    RecHeader header = w->node->header;
    const auto self = MakeRef(target, key);   // masters may have shifted
    if (!self) return fail(self.error().message);
    header.formId = *self;
    auto node = InsertRecord(target, header, rec.Encode());
    if (!node) return fail(node.error().message);
    return *node;
}

// ── Snapshot ─────────────────────────────────────────────────────────────────

std::string LoadOrderDocument::SnapshotJson() const {
    std::string out = "{\n";
    out += "  \"dataFolder\": " + JsonString(options_.dataFolder) + ",\n";
    out += "  \"pluginsTxt\": " + JsonString(options_.pluginsTxt) + ",\n";
    out += "  \"language\": " + JsonString(options_.language) + ",\n";
    out += "  \"plugins\": [";
    for (size_t i = 0; i < listing_.size(); ++i) {
        const PluginListing& l = listing_[i];
        out += i ? ",\n    {" : "\n    {";
        out += "\"filename\": " + JsonString(l.filename);
        out += ", \"loadIndex\": " + std::to_string(l.loadIndex);
        out += std::string(", \"master\": ") + (l.isMaster ? "true" : "false");
        out += std::string(", \"light\": ") + (l.isLight ? "true" : "false");
        out += std::string(", \"enabled\": ") + (l.enabled ? "true" : "false");
        out += std::string(", \"missing\": ") + (l.missing ? "true" : "false");
        if (const Plugin* p = FindPlugin(l.filename)) {
            out += ", \"localized\": ";
            out += p->localized ? "true" : "false";
            out += ", \"masters\": [";
            for (size_t m = 0; m < p->masters.size(); ++m)
                out += (m ? ", " : "") + JsonString(p->masters[m]);
            out += "]";
        }
        out += "}";
    }
    out += listing_.empty() ? "]\n" : "\n  ]\n";
    out += "}\n";
    return out;
}

} // namespace esper
