#include <ESPER/LoadOrderIndex.h>

#include <ESPER/CaseUtil.h>
#include <ESPER/PluginFlags.h>

#include <algorithm>

namespace esper {

static constexpr uint64_t kNoKey = ~0ull;

uint32_t LoadOrderIndex::InternName(const std::string& name) {
    const std::string lower = ToLowerCI(name);
    auto it = nameIds_.find(lower);
    if (it != nameIds_.end()) return it->second;
    const uint32_t id = static_cast<uint32_t>(names_.size());
    names_.push_back(name);            // canonical spelling = first seen
    nameIds_.emplace(lower, id);
    return id;
}

uint64_t LoadOrderIndex::FindKeyId(const FormKey& key) const {
    auto it = nameIds_.find(ToLowerCI(key.plugin));
    if (it == nameIds_.end()) return kNoKey;
    return (static_cast<uint64_t>(it->second) << 32) | (key.localId & 0x00FFFFFFu);
}

void LoadOrderIndex::Build(std::vector<std::unique_ptr<Plugin>> plugins) {
    plugins_ = std::move(plugins);
    names_.clear();
    nameIds_.clear();
    chains_.clear();
    winningBySig_.clear();
    regularOrder_.clear();
    eslOrder_.clear();

    for (const auto& pp : plugins_) {
        if (pp->isLight) eslOrder_.push_back(pp.get());
        else             regularOrder_.push_back(pp.get());
    }

    // Per plugin: intern [masters..., self] once, so per-record key derivation
    // is table lookups only (no string work on the ~10^6-record hot path).
    for (const auto& pp : plugins_) {
        Plugin* p = pp.get();

        std::vector<uint32_t> hiToName;
        hiToName.reserve(p->masters.size() + 1);
        for (const std::string& m : p->masters) hiToName.push_back(InternName(m));
        const uint32_t selfName = InternName(p->filename);

        for (const auto& [formId, node] : p->index) {
            const uint32_t hi      = formId >> 24;
            const uint32_t localId = formId & 0x00FFFFFFu;
            const uint32_t nameId  =
                (hi < hiToName.size()) ? hiToName[hi] : selfName;
            const uint64_t keyId =
                (static_cast<uint64_t>(nameId) << 32) | localId;

            ResolvedRecord rr;
            rr.owner = p;
            rr.node  = node;
            rr.key   = FormKey{ names_[nameId], localId };
            chains_[keyId].push_back(std::move(rr));
        }
    }

    // Winning-per-signature buckets, deterministically ordered. chains_ is
    // fully built (no more rehashing), so stored pointers are stable.
    for (const auto& [keyId, chain] : chains_) {
        (void)keyId;
        const ResolvedRecord& win = chain.back();
        if (win.Flags() & rec_flag::kDeleted) continue;
        winningBySig_[win.Sig()].push_back(&win);
    }
    for (auto& [s, bucket] : winningBySig_) {
        (void)s;
        std::sort(bucket.begin(), bucket.end(),
                  [](const ResolvedRecord* a, const ResolvedRecord* b) {
                      const int c = CompareCI(a->key.plugin, b->key.plugin);
                      if (c != 0) return c < 0;
                      return a->key.localId < b->key.localId;
                  });
    }
}

const ResolvedRecord* LoadOrderIndex::Resolve(const FormKey& key) const {
    const uint64_t keyId = FindKeyId(key);
    if (keyId == kNoKey) return nullptr;
    auto it = chains_.find(keyId);
    if (it == chains_.end() || it->second.empty()) return nullptr;
    const ResolvedRecord& win = it->second.back();
    if (win.Flags() & rec_flag::kDeleted) return nullptr;
    return &win;
}

const std::vector<ResolvedRecord>* LoadOrderIndex::ResolveChain(const FormKey& key) const {
    const uint64_t keyId = FindKeyId(key);
    if (keyId == kNoKey) return nullptr;
    auto it = chains_.find(keyId);
    return (it == chains_.end()) ? nullptr : &it->second;
}

const ResolvedRecord* LoadOrderIndex::ResolveRef(const Plugin* owner,
                                                 uint32_t formId,
                                                 uint32_t wantSig) const {
    if (!owner) return nullptr;
    const ResolvedRecord* rec = Resolve(owner->FormIdToKey(formId));
    if (!rec) return nullptr;
    if (wantSig != 0 && rec->Sig() != wantSig) return nullptr;
    return rec;
}

void LoadOrderIndex::EnumerateWinning(uint32_t sig,
                                      std::vector<const ResolvedRecord*>& out) const {
    auto it = winningBySig_.find(sig);
    if (it == winningBySig_.end()) return;
    out.insert(out.end(), it->second.begin(), it->second.end());
}

FormKey LoadOrderIndex::RuntimeFormIdToKey(uint32_t formId) const {
    const uint32_t hi = formId >> 24;

    if (hi == 0xFE) {
        const uint32_t slot     = (formId >> 12) & 0xFFFu;
        const uint32_t objectId =  formId        & 0xFFFu;
        if (slot < eslOrder_.size())
            return FormKey{ eslOrder_[slot]->filename, objectId };
        return FormKey{};
    }
    if (hi == 0xFF)   // runtime-created form; never file-backed
        return FormKey{};

    if (hi < regularOrder_.size())
        return FormKey{ regularOrder_[hi]->filename, formId & 0x00FFFFFFu };
    return FormKey{};
}

FormKey LoadOrderIndex::ResolveFormId(const Plugin* referencingPlugin,
                                      uint32_t formId) const {
    if (!referencingPlugin) return FormKey{};
    return referencingPlugin->FormIdToKey(formId);
}

} // namespace esper
