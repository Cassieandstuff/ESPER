#include <ESPER/WorldIndex.h>

#include <ESPER/GroupTree.h>
#include <ESPER/PluginFlags.h>
#include <ESPER/Subrecords.h>

#include <cstring>
#include <string>
#include <unordered_map>

namespace esper {

namespace {

// Light XCLC scan: exterior grid coords without a full CELL decode.
// Accepts the 8-byte (x,y) and 12-byte (x,y,flags) variants.
bool ScanCellGrid(RecordNode& cell, int32_t& outX, int32_t& outY) {
    char err[128] = {};
    const std::span<const uint8_t> payload = cell.Payload(err, sizeof(err));
    if (payload.data() == nullptr) return false;

    SubIterator it(payload);
    SubView sub;
    while (it.Next(sub)) {
        if (sub.sig == Sig4("XCLC") && sub.size >= 8) {
            std::memcpy(&outX, sub.data, 4);
            std::memcpy(&outY, sub.data + 4, 4);
            return true;
        }
    }
    return false;
}

} // namespace

void WorldIndex::Build(const LoadOrderIndex& index) {
    worlds_.clear();
    interiors_.clear();

    struct Walker {
        WorldIndex&           self;
        const LoadOrderIndex& index;
        Plugin*               plugin;

        void Walk(std::vector<Node>& nodes, bool inWorld,
                  const FormKey& worldKey, uint64_t worldKeyId) {
            for (size_t i = 0; i < nodes.size(); ++i) {
                Node& n = nodes[i];
                if (n.IsGroup()) {
                    GroupNode& g = *n.group;
                    if (g.header.groupType == group_type::kWorldChildren) {
                        const FormKey wk =
                            plugin->FormIdToKey(g.header.label);
                        Walk(g.children, true, wk, index.KeyId(wk));
                    } else {
                        Walk(g.children, inWorld, worldKey, worldKeyId);
                    }
                    continue;
                }

                RecordNode& rec = n.rec;
                if (rec.header.sig != sig::CELL) continue;

                const FormKey  key   = plugin->FormIdToKey(rec.header.formId);
                const uint64_t keyId = index.KeyId(key);
                if (keyId == LoadOrderIndex::kNoKeyId) continue;

                // The cell-children group is the immediately following
                // sibling GRUP whose label is this cell's on-disk FormID.
                GroupNode* children = nullptr;
                if (i + 1 < nodes.size() && nodes[i + 1].IsGroup()) {
                    GroupNode& sib = *nodes[i + 1].group;
                    if (sib.header.groupType == group_type::kCellChildren &&
                        sib.header.label == rec.header.formId)
                        children = &sib;
                }

                GroupNode* persistent = nullptr;
                GroupNode* temporary  = nullptr;
                RecordNode* land      = nullptr;
                if (children) {
                    for (Node& c : children->children) {
                        if (!c.IsGroup()) continue;
                        GroupNode& cg = *c.group;
                        if (cg.header.groupType == group_type::kCellPersistent)
                            persistent = &cg;
                        else if (cg.header.groupType == group_type::kCellTemporary ||
                                 cg.header.groupType == group_type::kCellVisibleDistant)
                            temporary = &cg;
                    }
                    if (temporary)
                        for (Node& c : temporary->children)
                            if (!c.IsGroup() && c.rec.header.sig == Sig4("LAND"))
                                land = &c.rec;
                }

                int32_t x = 0, y = 0;
                const bool exterior = inWorld && ScanCellGrid(rec, x, y);

                CellEntry* entry;
                if (exterior) {
                    entry = &self.worlds_[worldKeyId][PackXY(x, y)];
                    entry->exterior = true;
                    entry->x        = x;
                    entry->y        = y;
                    entry->worldKey = worldKey;
                } else {
                    entry = &self.interiors_[keyId];
                }
                entry->key   = key;
                entry->owner = plugin;
                entry->cell  = &rec;
                // Children aspects only update when THIS plugin provides them,
                // so a record-only override keeps the base game's refs/LAND.
                // Every provider is appended as a layer (in load order) so refs
                // union across the whole load order; childOwner/persistent/
                // temporary/land track the winning (last) provider for LAND.
                if (children) {
                    entry->childOwner = plugin;
                    entry->persistent = persistent;
                    entry->temporary  = temporary;
                    entry->childLayers.push_back({ plugin, persistent, temporary });
                    if (land) entry->land = land;
                }
            }
        }
    };

    for (const auto& pp : index.Plugins()) {
        Walker w{ *this, index, pp.get() };
        w.Walk(pp->tree, false, FormKey{}, LoadOrderIndex::kNoKeyId);
    }
}

const CellEntry* WorldIndex::FindExterior(const LoadOrderIndex& index,
                                          const FormKey& worldKey,
                                          int32_t x, int32_t y) const {
    const uint64_t wid = index.KeyId(worldKey);
    auto wit = worlds_.find(wid);
    if (wit == worlds_.end()) return nullptr;
    auto cit = wit->second.find(PackXY(x, y));
    return (cit == wit->second.end()) ? nullptr : &cit->second;
}

const CellEntry* WorldIndex::FindInterior(const LoadOrderIndex& index,
                                          const FormKey& cellKey) const {
    auto it = interiors_.find(index.KeyId(cellKey));
    return (it == interiors_.end()) ? nullptr : &it->second;
}

void WorldIndex::EnumerateExterior(const LoadOrderIndex& index,
                                   const FormKey& worldKey,
                                   std::vector<const CellEntry*>& out) const {
    const uint64_t wid = index.KeyId(worldKey);
    auto wit = worlds_.find(wid);
    if (wit == worlds_.end()) return;
    out.reserve(out.size() + wit->second.size());
    for (const auto& [xy, entry] : wit->second) {
        (void)xy;
        out.push_back(&entry);
    }
}

void WorldIndex::EnumerateInteriors(std::vector<const CellEntry*>& out) const {
    out.reserve(out.size() + interiors_.size());
    for (const auto& [keyId, entry] : interiors_) {
        (void)keyId;
        out.push_back(&entry);
    }
}

void WorldIndex::CollectPlacedRefs(const CellEntry& cell,
                                   std::vector<PlacedRef>& out) {
    // Union placed refs across every plugin's cell-children by winning FormKey:
    // later load-order layers override earlier ones (base architecture stays put
    // while a mod's edited furniture replaces its own entries), and a Deleted
    // override removes the ref entirely. First-seen order is preserved so the
    // base game's layout order survives.
    std::vector<PlacedRef> order;
    std::unordered_map<std::string, size_t> pos;   // FormKey canonical -> index in order
    auto add = [&](GroupNode* g, const Plugin* owner) {
        if (!g || !owner) return;
        for (Node& n : g->children) {
            if (n.IsGroup()) continue;
            const uint32_t sig = n.rec.header.sig;
            if (sig == Sig4("LAND") || sig == Sig4("NAVM")) continue;
            const std::string key = owner->FormIdToKey(n.rec.header.formId).ToString();
            auto it = pos.find(key);
            if (n.rec.Deleted()) {                        // override deletes the ref
                if (it != pos.end()) order[it->second].rec = nullptr;
                continue;
            }
            if (it == pos.end()) {
                pos.emplace(key, order.size());
                order.push_back({ &n.rec, owner });
            } else {
                order[it->second] = { &n.rec, owner };    // winner replaces in place
            }
        }
    };
    for (const CellChildLayer& layer : cell.childLayers) {
        add(layer.persistent, layer.owner);
        add(layer.temporary,  layer.owner);
    }
    for (const PlacedRef& r : order)
        if (r.rec) out.push_back(r);
}

size_t WorldIndex::ExteriorCellCount() const {
    size_t n = 0;
    for (const auto& [w, cells] : worlds_) {
        (void)w;
        n += cells.size();
    }
    return n;
}

} // namespace esper
