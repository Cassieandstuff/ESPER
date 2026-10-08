#pragma once
#include <ESPER/FormKey.h>
#include <ESPER/Plugin.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace esper {

// One version of a record in an override chain. `node` points into the owning
// Plugin's tree — valid while the index is alive. The node pointer is
// deliberately non-const so consumers can call RecordNode::Payload() (lazy
// decompression) through a const ResolvedRecord.
struct ResolvedRecord {
    const Plugin* owner = nullptr;
    RecordNode*   node  = nullptr;
    FormKey       key;              // defining plugin + 24-bit local id

    uint32_t Sig() const   { return node->header.sig; }
    uint32_t Flags() const { return node->header.flags; }
};

// Owns loaded Plugins in load order and maintains per-FormKey override chains.
// The LAST element of a chain is the winner (highest-priority plugin's copy).
// Correctness rests on Build() receiving plugins in true load order —
// LoadOrderReader::ResolveOrder produces that order (masters hoisted).
class LoadOrderIndex {
public:
    void Build(std::vector<std::unique_ptr<Plugin>> plugins);

    // Winning version, or nullptr if unknown / the winner is deleted.
    const ResolvedRecord* Resolve(const FormKey& key) const;

    // Full chain oldest→newest (no filtering), or nullptr if unknown.
    const std::vector<ResolvedRecord>* ResolveChain(const FormKey& key) const;

    // FormIdToKey through `owner`'s master list, then Resolve, then an optional
    // signature filter (wantSig == 0 accepts any signature).
    const ResolvedRecord* ResolveRef(const Plugin* owner, uint32_t formId,
                                     uint32_t wantSig) const;

    // Winning records with signature == `sig`, in deterministic order
    // (case-insensitive defining-plugin name, then local id). DELETED winners
    // are omitted.
    void EnumerateWinning(uint32_t sig, std::vector<const ResolvedRecord*>& out) const;

    size_t PluginCount() const { return plugins_.size(); }
    const std::vector<std::unique_ptr<Plugin>>& Plugins() const { return plugins_; }

    // Runtime load-order slots: non-light plugins occupy the regular index
    // bytes 0x00..0xFD in order; light plugins occupy the 0xFE slot space.
    const std::vector<const Plugin*>& RegularOrder() const { return regularOrder_; }
    const std::vector<const Plugin*>& EslOrder() const     { return eslOrder_; }

    // Maps a RUNTIME FormID (console/save-file style: 0xFE-prefixed light
    // space, regular load-order index bytes) to a FormKey. This is distinct
    // from Plugin::FormIdToKey, which implements the ON-DISK master-index
    // rule — 0xFE never appears on disk.
    FormKey RuntimeFormIdToKey(uint32_t formId) const;

    FormKey ResolveFormId(const Plugin* referencingPlugin, uint32_t formId) const;

    // Stable interned key for a FormKey: (name id << 32) | 24-bit local id.
    // kNoKeyId when the plugin name is unknown to this index. Useful for
    // consumers (WorldIndex) that key their own maps by record identity.
    static constexpr uint64_t kNoKeyId = ~0ull;
    uint64_t KeyId(const FormKey& key) const { return FindKeyId(key); }
    uint64_t KeyIdFor(const Plugin& owner, uint32_t formId) const {
        return FindKeyId(owner.FormIdToKey(formId));
    }

private:
    // Key: (interned defining-plugin name id << 32) | 24-bit local id.
    uint32_t InternName(const std::string& name);           // build-time
    uint64_t FindKeyId(const FormKey& key) const;           // kNoKeyId if unknown

    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<std::string>                  names_;   // nameId -> canonical name
    std::unordered_map<std::string, uint32_t> nameIds_; // lowercased -> nameId

    std::unordered_map<uint64_t, std::vector<ResolvedRecord>> chains_;
    std::unordered_map<uint32_t, std::vector<const ResolvedRecord*>> winningBySig_;

    std::vector<const Plugin*> regularOrder_;
    std::vector<const Plugin*> eslOrder_;
};

} // namespace esper
