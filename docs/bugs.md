# ESPER — bug log

Symptom · what's ruled out · root cause · exact `file:line` · status (`OPEN`/`INVESTIGATING`/`FIXED`).
Keep in sync in the SAME change as the code (CLAUDE.md directive).

## Open

### FormID remap silently skips FormIDs inside `Blob`-bound subrecords — OPEN
- **Symptom:** `SchemaOps::remapIds` accepts records whose opaque blobs contain FormIDs, visits none of
  those FormIDs, and returns success. Every caller that renumbers masters, including
  `EnsureMaster`/`SelfIndexBump`, then leaves those references pointing at the old master index.
  The result is a silently wrong reference. Reproduced 2026-10-08 with ECZN: `DATA` = owner
  `0x01000ABC` + location `0x01000DEF`, and a remap of index 1 → 2 gives
  `accepted=1 visited=0`, with the bytes unchanged.
- **Root cause:** `Blob` and `ClmtBlob` always decode successfully, so the subrecord counts as
  *consumed*, not unknown. Their rows have no `visitIds` and no `remapSafe` hook.
  `RemapIdsImpl` ([SchemaRegistry.cpp](../cpp/schema/SchemaRegistry.cpp)) refuses only on unknown
  subrecords or a `remapSafe` failure, so it treats these records as fully visited. The header
  comments for these rows say "→ passthrough (so remap refuses)", but the code binds them as `Blob`.
- **Affected rows:**
  - confirmed by repro: [RecordsSystem.h:243](../include/ESPER/RecordsSystem.h) ECZN `DATA`;
  - FormIDs per the code's own comments:
    - [RecordsMore.h:408](../include/ESPER/RecordsMore.h) RELA `DATA`
    - [RecordsMore.h:436](../include/ESPER/RecordsMore.h) PWAT `DNAM`
    - [RecordsMore.h:466](../include/ESPER/RecordsMore.h) NAVM `NVNM`
    - [RecordsSystem.h:289](../include/ESPER/RecordsSystem.h) CLMT `WLST` (`ClmtBlob`)
  - The other ~110 `Blob` rows have not been audited.
- **Fix direction:**
  - Short term: an opaque-blob binder whose `remapSafe` returns false whenever the field is
    present, used for every blob not known to be FormID-free.
  - Schema design: a non-empty `bytes` field is remap-unsafe unless the descriptor declares
    `formIds: none` (see [schema-design.md](schema-design.md)).

## Fixed

### EnsureMaster renumbered NULL FormIDs on masterless plugins — FIXED
- **Symptom:** appending the FIRST master to a plugin (oldSelf index 0) rewrote every null (0)
  FormID reference to `0x01000000` — null refs ("no enchantment", "no template") became dangling
  references into the new master.
- **Root cause:** `SelfIndexBump::Apply` matched on `(id >> 24) == oldSelf` without excluding the
  null FormID, and a masterless plugin's self index IS 0.
- **Fix:** null FormIDs are never renumbered
  ([Writer.cpp](../cpp/Writer.cpp) `SelfIndexBump::Apply`).
  Found during the CTDA remap-safety review (same reasoning: integers that look like index-0
  FormIDs must not be touched); fixed in the wave-4/conditions change.

### ESL 0xFE FormIDs resolved through a global slot table instead of the owner's master list — FIXED
- **Symptom:** references into light masters could silently resolve to the wrong plugin whenever
  the loaded set didn't exactly match the game's light-plugin order.
- **Root cause:** on disk, a FormID's top byte is ALWAYS an index into the owning plugin's
  `[MAST list…, self]` — the 0xFE prefix is a *runtime* load-order construct. The old
  `Plugin::FormIdToKey` treated on-disk 0xFE as a lookup into a global "all light plugins in load
  order" table (`eslTable`), which was additionally null during `Plugin::Load`, so 0xFE
  self-FormIDs keyed to an empty plugin name and collided globally.
- **Fix:** `FormIdToKey` implements the master-index rule only
  ([Plugin.cpp](../cpp/Plugin.cpp) `FormIdToKey`); the runtime FE
  mapping moved to `LoadOrderIndex::RuntimeFormIdToKey`
  ([LoadOrderIndex.cpp](../cpp/LoadOrderIndex.cpp)). The old
  behavior was also baked into the M7 test fixture (0xFE-packed on-disk refs) — rewritten to
  master-index refs. Fixed in the Phase-0 container rewrite (bethesda-plugins parity work).

### No ESM-before-ESP hoist in load-order resolution — FIXED
- **Symptom:** wrong override winners whenever plugins.txt listed a master-flagged plugin after a
  non-master (`LoadOrderIndex` derives winners purely from input order).
- **Fix:** `LoadOrderReader::ResolveOrder` reads each present plugin's TES4 flags (extension
  forcing: `.esm` → master, `.esl` → master+light; ESPFE does NOT hoist) and stable-partitions
  masters first ([LoadOrderReader.cpp](../cpp/LoadOrderReader.cpp)).

### Skyrim.ccc re-enabled plugins the user disabled in plugins.txt — FIXED
- **Symptom:** a Creation Club plugin unstarred in plugins.txt still loaded, because the ccc pass
  ran BEFORE plugins.txt and the case-insensitive dedup keeps the first occurrence.
- **Fix:** plugins.txt is read first; Skyrim.ccc only appends entries plugins.txt doesn't mention
  (same file as above, phases 2–3).

### Only 6 hardcoded record signatures indexed; GRUP semantics discarded — FIXED
- **Symptom:** `IsTargetSig` limited the index to NPC_/RACE/ARMO/ARMA/HDPT/TXST; `WalkGroup`
  ignored `GrupHeader.label`/`groupType`, so group structure was unrecoverable (blocking any
  writer and all CELL/WRLD work); a malformed group aborted its whole level.
- **Fix:** full structure-preserving group tree + index over EVERY record with per-node
  diagnostics ([Plugin.cpp](../cpp/Plugin.cpp) `WalkLevel`,
  [GroupTree.h](../include/ESPER/GroupTree.h)). Verified by the
  round-trip gate: all five vanilla masters + AE CC ESLs re-emit byte-identically
  ([roundtrip_gate.cpp](../tools/roundtrip_gate.cpp)).
