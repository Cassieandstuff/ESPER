#pragma once
#include <ESPER/Plugin.h>
#include <ESPER/Schema.h>
#include <ESPER/Zlib.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace esper {

// ─────────────────────────────────────────────────────────────────────────────
// Serialization (Phase 1) — verbatim-unless-modified.
// ─────────────────────────────────────────────────────────────────────────────

// Serializes a Plugin's parse tree back to plugin bytes. An unmodified record
// emits its original 24-byte header + raw payload untouched (compressed blobs
// carried through as-is), so a freshly parsed plugin re-serializes
// byte-identically. A record with `modified` set emits the replacement
// payload with dataSize updated. GRUP headers keep their label/type/stamp
// fields and get groupSize recomputed bottom-up.
bool SerializePlugin(const Plugin& p, std::vector<uint8_t>& out,
                     char* err, int errLen);

// SerializePlugin + write to disk.
bool SavePlugin(const Plugin& p, const std::string& path, char* err, int errLen);

// ─────────────────────────────────────────────────────────────────────────────
// Mutation (Phase 3) — in-place edits on a Plugin the caller owns.
//
// IMPORTANT: mutate only a standalone Plugin (e.g. one opened for a project),
// never one owned by a live LoadOrderIndex — the index holds node pointers
// that structural edits (CreateRecord) invalidate and RebuildIndex only
// repairs plugin-locally.
// ─────────────────────────────────────────────────────────────────────────────

// Initializes `p` as a fresh, empty in-memory plugin (no file behind it).
// `light` sets the ESL flag (kMaster is implied for .esl-named files by the
// loader; here only the flag matters). FormID allocation starts at 0x800.
void CreateEmptyPlugin(Plugin& p, const std::string& filename, bool light);

// Allocates the next object id from HEDR.nextObjectId and returns the on-disk
// self-index FormID. Returns 0 on ESL object-id exhaustion (> 0xFFF).
uint32_t AllocateFormId(Plugin& p, char* err, int errLen);

// Appends `masterName` to the MAST list if absent and returns its index.
// Appending shifts the self index, so every existing record is renumbered:
// record headers mechanically, payload references through their schema's
// remapIds (which refuses records with unknown-passthrough subrecords).
// Returns -1 on failure.
int EnsureMaster(Plugin& p, const std::string& masterName, char* err, int errLen);

// Builds the on-disk FormID for a reference to `key` from inside `p`:
// master-index top byte (adding the master via EnsureMaster if needed) or
// self. Returns 0 on failure.
uint32_t MakeRef(Plugin& p, const FormKey& key, char* err, int errLen);

// Creates a new record of `sig` with a freshly allocated FormID, placed in
// (creating if needed) the type-0 top group for `sig`. Returns the node, or
// nullptr on failure. Invalidates previously held RecordNode pointers into
// this plugin (the index is rebuilt).
RecordNode* CreateRecord(Plugin& p, uint32_t sig, char* err, int errLen);

// Re-derives `p.index` from the tree (after structural edits).
void RebuildIndex(Plugin& p);

// Replaces a record's payload with `payload`, honoring the record's
// compression flag (a compressed record is re-deflated; bytes will differ
// from Bethesda's zlib — the gate's compressed criterion is
// decompressed-payload equality). Marks the node dirty.
bool SetRecordPayload(Plugin& p, RecordNode& node,
                      std::vector<uint8_t> payload, char* err, int errLen);

// Typed edit round-trip over one node: decode via `schema` (empty payload →
// default-initialized record), mutate, then CommitNode to write back.
template <class Rec, size_t N>
bool DecodeNode(const RecordSchema<Rec, N>& schema, const Plugin& p,
                RecordNode& node, Rec& out, char* err, int errLen) {
    const std::span<const uint8_t> payload = node.Payload(err, errLen);
    if (payload.data() == nullptr) return false;
    SchemaCtx ctx;
    ctx.localized = p.localized;
    if (!DecodeRecord(schema, payload, ctx, out)) {
        std::snprintf(err, errLen, "DecodeNode: malformed subrecord stream in %08X",
                      node.header.formId);
        return false;
    }
    return true;
}

template <class Rec, size_t N>
bool CommitNode(const RecordSchema<Rec, N>& schema, Plugin& p,
                RecordNode& node, const Rec& rec, char* err, int errLen) {
    SchemaCtx ctx;
    ctx.localized = p.localized;
    ByteWriter w;
    EncodeRecord(schema, rec, ctx, w);
    return SetRecordPayload(p, node, std::move(w.buf), err, errLen);
}

// Save-time finalization: rebuilds the TES4 payload from the Plugin's decoded
// fields (HEDR with the empirically locked numRecords = records + groups
// rule, CNAM/SNAM, MAST+DATA, ONAM, INTV/INCC) and validates ESL object-id
// constraints. Call before SavePlugin on any plugin that was edited; a
// pristine parsed plugin skips this to stay byte-identical.
bool FinalizePlugin(Plugin& p, char* err, int errLen);

} // namespace esper
