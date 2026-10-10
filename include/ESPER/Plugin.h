#pragma once
#include <ESPER/Error.h>
#include <ESPER/FormKey.h>
#include <ESPER/GroupTree.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace esper {

// A single ESM/ESP/ESL plugin.
//
// OpenHeader() does a lightweight TES4-only parse (a few KB): flags, HEDR,
// author/description, masters. Load() reads the ENTIRE file and builds the
// structure-preserving group tree (`tree`) plus a full record index over EVERY
// record (`index`). Record payloads are zero-copy views into `buffer`; the
// Plugin is therefore non-copyable (moves are fine — vector heap storage keeps
// node addresses and payload pointers stable).
struct Plugin {
    std::string filename;

    // TES4 flags.
    bool isMaster  = false;
    bool isLight   = false;
    bool localized = false;

    // Decoded TES4 fields (the raw TES4 record is preserved in `tes4`).
    float    hedrVersion      = 0.f;   // HEDR
    int32_t  hedrNumRecords   = 0;     // HEDR (counting rule verified empirically)
    uint32_t hedrNextObjectId = 0;     // HEDR
    std::string author;                // CNAM
    std::string description;           // SNAM
    std::vector<std::string> masters;  // MAST, in declaration order
    std::vector<uint64_t>    masterData; // the u64 DATA paired with each MAST
    std::vector<uint32_t>    onam;     // overridden-forms list (preserved verbatim)
    bool hasIntv = false; uint32_t intv = 0;
    bool hasIncc = false; uint32_t incc = 0;

    // Set by the mutation API (Writer.h); SavePlugin finalizes an edited
    // plugin's TES4 before writing.
    bool edited = false;

    // Whole file, kept for zero-copy payloads and verbatim re-emission.
    std::vector<uint8_t> buffer;

    // The TES4 record itself (raw view into buffer; never compressed).
    RecordNode tes4;

    // Top-level nodes in file order (normally all groups; stray top-level
    // records are tolerated with a diagnostic).
    std::vector<Node> tree;

    // EVERY record in the file, keyed by its on-disk (raw) FormID. Duplicate
    // FormIDs within one file are a diagnostic; last occurrence wins.
    std::unordered_map<uint32_t, RecordNode*> index;

    // Malformed-structure reports from the walk (empty for well-formed files).
    std::vector<Diagnostic> diagnostics;

    Plugin() = default;
    Plugin(const Plugin&)            = delete;
    Plugin& operator=(const Plugin&) = delete;
    Plugin(Plugin&&)                 = default;
    Plugin& operator=(Plugin&&)      = default;

    // Both set `filename` to the path's file name; set it again afterwards to
    // load a file under a different name.
    Result<void> OpenHeader(const std::string& path);
    Result<void> Load(const std::string& path);

    // The record with on-disk FormID `formId`, or nullptr.
    RecordNode* Find(uint32_t formId) const;
    // Every record, or every record of `sig`, in file order (groups flattened).
    std::vector<RecordNode*> Records() const;
    std::vector<RecordNode*> Records(uint32_t sig) const;

    // Resolves a 32-bit FormID found INSIDE this plugin to a FormKey, per the
    // ON-DISK rule: the top byte indexes [masters..., self] (any index at or
    // beyond the master count means self — matching engine behavior). The
    // local id is always the low 24 bits; the ESL 12-bit object-id ceiling is
    // a write-side validation, not a read-side mask. The 0xFE prefix is a
    // RUNTIME load-order construct and never appears here — see
    // LoadOrderIndex::RuntimeFormIdToKey.
    FormKey FormIdToKey(uint32_t formId) const;
};

} // namespace esper
