#pragma once
#include <ESPER/LoadOrderIndex.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace esper {

// One plugin's contribution to a cell's children — the persistent/temporary
// groups AND the plugin that owns them (needed to resolve each ref's local
// FormID). One layer per plugin that provides a cell-children GRUP, kept in
// load order so CollectPlacedRefs can union refs across the whole load order.
struct CellChildLayer {
    const Plugin* owner      = nullptr;
    GroupNode*    persistent = nullptr;   // group type 8
    GroupNode*    temporary  = nullptr;   // group type 9
};

// A placed reference plus the plugin whose FormID space it lives in. After the
// cross-plugin union a cell's refs come from MANY plugins (base architecture +
// a mod's edited furniture), so the owner can't be a single cell-wide value.
struct PlacedRef {
    RecordNode*   rec   = nullptr;
    const Plugin* owner = nullptr;
};

// One cell's structural home after load-order merge. Node/group pointers
// reach into the OWNING plugin's tree — valid while the LoadOrderIndex lives.
// Placed refs are unioned across every plugin's children (see childLayers /
// CollectPlacedRefs); LAND keeps the winning provider (childOwner).
struct CellEntry {
    FormKey       key;                    // cell identity
    const Plugin* owner      = nullptr;   // winning CELL provider
    RecordNode*   cell       = nullptr;
    const Plugin* childOwner = nullptr;   // winning (last) children provider — for LAND
    GroupNode*    persistent = nullptr;   // winning layer's group type 8
    GroupNode*    temporary  = nullptr;   // winning layer's group type 9
    std::vector<CellChildLayer> childLayers;  // every provider, in load order
    RecordNode*   land       = nullptr;   // LAND in temporary children
    int32_t       x = 0, y = 0;           // exterior grid coords
    bool          exterior = false;
    FormKey       worldKey;               // exterior only
};

// Override-aware WRLD → (x,y) → CELL index plus interior-cell lookup and
// O(tiles) terrain enumeration — the access patterns the editor's cell
// browser and terrain streamer need.
class WorldIndex {
public:
    // Walks every plugin's group tree once (decoding each exterior CELL's
    // XCLC — lazy record decompression happens here for cells).
    void Build(const LoadOrderIndex& index);

    // Winning WRLD records, deterministic order (delegates to the index).
    void EnumerateWorldspaces(const LoadOrderIndex& index,
                              std::vector<const ResolvedRecord*>& out) const {
        index.EnumerateWinning(Sig4("WRLD"), out);
    }

    const CellEntry* FindExterior(const LoadOrderIndex& index,
                                  const FormKey& worldKey,
                                  int32_t x, int32_t y) const;
    const CellEntry* FindInterior(const LoadOrderIndex& index,
                                  const FormKey& cellKey) const;

    // Every exterior cell of a worldspace (winning versions), unordered.
    void EnumerateExterior(const LoadOrderIndex& index, const FormKey& worldKey,
                           std::vector<const CellEntry*>& out) const;
    void EnumerateInteriors(std::vector<const CellEntry*>& out) const;

    // Collects the placed-reference records (REFR/ACHR/and other placed sigs)
    // from a cell, UNIONED across every plugin's persistent + temporary
    // children by winning FormKey — later load-order layers override earlier
    // ones, and a Deleted override removes the ref. Each result carries its
    // owning plugin (refs span multiple FormID spaces after the union).
    static void CollectPlacedRefs(const CellEntry& cell,
                                  std::vector<PlacedRef>& out);

    size_t ExteriorCellCount() const;
    size_t InteriorCellCount() const { return interiors_.size(); }

private:
    static uint64_t PackXY(int32_t x, int32_t y) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(y);
    }

    // worldKeyId -> packed (x,y) -> entry
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, CellEntry>> worlds_;
    // cellKeyId -> entry (cells without XCLC, incl. worldspace dummy cells)
    std::unordered_map<uint64_t, CellEntry> interiors_;
};

} // namespace esper
