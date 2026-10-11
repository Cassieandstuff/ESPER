# ESPER — bug log

Symptom · what's ruled out · root cause · exact `file:line` · status (`OPEN`/`INVESTIGATING`/`FIXED`).
Keep in sync in the SAME change as the code (CLAUDE.md directive).

## Open

None.

## Fixed

### Decoding struct-heavy records is several times slower than the deleted typed path — FIXED
- **Symptom:** found 2026-10-10 porting SCT. Decoding one vanilla LAND takes about 150 µs
  (Skyrim.esm, 4,000 records), and SCT's terrain query costs about 0.45 ms per cell, against
  about 0.08 ms before the port. Loading every Tamriel cell would take about 5 s instead of
  about 1 s. RACE takes about 100 µs per record.
- **Ruled out:**
  - Arrays of plain numbers. They're now stored packed (one `Value::bytes`, not a `Value` per
    element), which took LAND from 198 to 147 µs, together with reserving struct and array
    storage.
  - Repeated decodes in SCT. sct-esm now caches its dependency records.
- **Root cause:** every struct field and every element of a struct array is its own `Value`:
  ~96 bytes, holding a string and two vectors, so typically one or two heap allocations. LAND's
  `VTXT` alpha cells (`LandAlphaCell[]`, thousands per record) dominate.
  ([Record.h](../include/ESPER/Record.h) `Value`;
  [Record.cpp](../cpp/engine/Record.cpp) `Parse`.)
- **Fix:** three changes, measured on Skyrim.esm (decode, µs per record, before → after):
  LAND 198 → 4.6, RACE 99 → 38, NPC_ 14.8 → 5.2, ARMO 3.9 → 1.9, QUST 26.6 → 14.6.
  - **Packed plain arrays.** Any array whose element is plain (numbers, and fixed
    structs/arrays of them; generator-computed `TypeDef::plainSize`) is stored as raw bytes.
    Paths into one element still work: the engine unpacks the addressed part. Views read and
    write such arrays as `std::vector` of generated packed value structs
    (`view::value::LandAlphaCell`), copied straight from the bytes.
  - **Per-instance index.** Each instance's nodes and child instances, in document order, are
    rebuilt by Decode and after structural edits. Lookups touch their own instance instead of
    scanning the record: RACE head data through views went from 13.7 to 0.37 µs. Lookups no
    longer write any `mutable` state, so concurrent reads of one Record are safe again.
  - **Decode loop.** Nodes are moved, not copied. Occurrence counts are a flat array, not a
    hash map. The parse context is reused. Each scope's entry signatures are sorted
    (schemagen) and binary-searched.

  Gate: identical totals, byte-exact; the full schema gate runs in 8.7 s. In SCT (sct-esm,
  vanilla load order), single lookups are now faster than before the port. One NPC search
  takes 50 ms against 131, and `GetNpc` 15 ms against 26. Terrain (3 vs 2 ms per 25 cells),
  armor search and cells are at parity. Only listing every record is still slower: all NPCs
  270 ms against 199, and all armor 17 ms against 7. Those decode each record in full, where
  the old code read a few raw subrecords.

### Two different groups could generate one view class, silently dropping one — FIXED
- **Symptom:** an inline group whose class name (record name + group name) equals a reusable
  group's name produced a single view class. One group's accessors were lost, and accessors
  typed with that name pointed at the wrong group. Found 2026-10-10 modeling RACE: the inline
  `headData` section (class `RaceHeadData`) collided with a reusable group of that name, so
  `RaceHeadData::Male()` returned the section's own view instead of one sex's head data.
- **Root cause:** `check_view_names` ([schemagen.py](../tools/schemagen/schemagen.py)) claimed
  a group as `group <name>`, so two distinct groups with the same name looked identical. It
  also skipped delimited groups entirely (it only claimed `ordered` ones). `emit_views` dedups
  classes by name.
- **Fix:** groups record their origin (the file or member that defines them). Every non-root
  group claims its class name with that origin, so every use of one reusable definition shares a
  class, and two definitions can't. `schemagen_test` covers it; the RACE group is now `RaceHead`.

### Typed views exposed only the first occurrence of a repeated struct, and arrays of structs as untyped values — FIXED
- **Symptom:** the generated view for a repeated struct member returned one struct view, the
  first occurrence. Later occurrences were unreachable through views. LAND has several `BTXT`
  base textures and `ATXT` alpha layers per record. An array of structs (LAND `VTXT`, a list of
  `LandAlphaCell`) came back as `std::vector<esper::Value>`. Found 2026-10-10 while moving the
  gate's LAND check off the typed path, which had both as typed vectors.
- **Root cause:** the generator's struct branch ([schemagen.py](../tools/schemagen/schemagen.py),
  `emit_views` / `accessors`) ignored `repeat`. Paths had no way to name an occurrence or an
  array element: `FindValue`/`StoreValue` always used the first node, and `Descend` stepped only
  through structs and unions.
- **Fix (step 8):**
  - `RelPath::occurrence` names an occurrence of a repeated member.
  - Paths step into array elements (`Descend`, `TypeAt`, `StoreValue`).
  - Engine primitives `CountAt`/`AppendAt`/`ResizeAt`, exposed on `Cursor` as
    `Count`/`Append`/`Size`/`Resize`.
  - Repeated struct members generate `std::vector<View> X()` and `View AddX()`.
  - Arrays of structs generate element views and `ResizeX(n)`; repeated ones generate
    `std::vector<std::vector<View>> X()` and `AddX(n)`.
  - `esper-schema-test` T17 covers read, in-place write of a later occurrence or element, and
    authoring.

### FormID remap silently skips FormIDs inside `Blob`-bound subrecords — FIXED
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
  - the `MODS`/`MO2S`/`MO4S` alternate-texture blobs (the typed path's `modelSwap` rows): confirmed,
    see Measured below.
  - The other ~110 `Blob` rows have not been audited.
- **Scale (2026-10-09, `roundtrip-gate --schema` over the five vanilla masters):** the typed path
  would remap **98,384** vanilla records that the schema engine refuses, because they carry
  opaque bytes it can't see into. These are counted by the first blocker per record. The top
  blockers are `CTDA` (26,506), `MHDT` (24,019), `NVNM` (19,440), `TVDT` (8,595), `VMAD`
  (4,369), and various `DATA`/`DNAM`/`ONAM` blobs. `CTDA` and `VMAD` are typed-path codecs that
  *do* see FormIDs, so those refusals are the new engine being conservative until unions land,
  not typed-path bugs. Every other blocker is an unchecked `Blob`, which is this bug's
  territory.
- **Update (2026-10-09, step 4):** CTDA and VMAD are now typed in the schema, with FormID parity
  against the typed path. The engine's remap refusals fell to **68,299**, and every remaining
  blocker is an unchecked `Blob`: `MHDT` 24,019, `NVNM` 19,440, `TVDT` 8,595, then
  `DATA`/`DNAM`/`ONAM`/`MODS`/`MNAM`.
- **Measured (2026-10-09, step 4 research).** The schema now models these subrecords with typed
  FormID fields, and every `to:` target is verified on the vanilla masters. `roundtrip-gate --schema`
  compares their FormIDs with what the typed path's remap visits. On the vanilla masters the typed
  path **silently skips 7,250 FormIDs in 5,590 records**:

  | Rows | Records | FormIDs skipped | What they reference |
  |---|---|---|---|
  | SNDR `ONAM` | 3,197 | 3,197 | SOPM (output model) |
  | RELA `DATA` | 673 | 1,776 | NPC_ parent and child, ASTP association type |
  | `MODS`/`MO2S`/`MO4S` in STAT, ARMO, ARMA, ALCH, MISC, WEAP | 1,382 | 1,938 | TXST (alternate textures) |
  | ECZN `DATA` | 338 | 339 | FACT/NPC_ owner, LCTN location |

  NAVM `NVNM` and CLMT `WLST` are still opaque in the schema too, so the engine refuses to remap
  them, and they aren't counted here.
- **Fix direction:**
  - Short term: an opaque-blob binder whose `remapSafe` returns false whenever the field is
    present, used for every blob not known to be FormID-free.
  - Schema design: a non-empty `bytes` field is remap-unsafe unless the descriptor declares
    `formIds: none` (see [schema-design.md](schema-design.md)).
- **Fix (step 8, the typed path deleted):** `SchemaOps::remapIds` and every `Blob` row are gone.
  `EnsureMaster` now decodes each record with the schema engine and renumbers through
  `Record::VisitFormIds`. That refuses a record with a raw subrecord, a non-empty `bytes` value
  not declared `formIds: none`, or a `remapUnsafe` value, so an unseen FormID can no longer be
  skipped. `OverrideInto` already used the engine. The rows measured above are typed FormIDs in
  the schema now (SNDR `ONAM`, RELA/ECZN `DATA`, `MODS`), with `to:` targets the gate verifies.
  `esper-test` T12 checks that `EnsureMaster` refuses an unclassifiable condition parameter.

### A handle or view for one record type reads another record type's fields silently — FIXED
- **Symptom:** you can apply a `fields::<SIG>::…` handle or a `view::<Name>` to a `Record` of a
  different type. Neither checks the type, and the call returns that record's field at the same
  member index, converted to the handle's type. Reproduced 2026-10-10 over Skyrim.esm's 5,118 NPC_
  records:
  - `rec.Get(fields::ARMO::race)` returned a "FormID" for all 5,118. It was usually `000000FF`,
    the raw value of whatever NPC_ field sits at ARMO's `race` index.
  - `view::Armor(npcRec).ArmorRating()` returned a value for 548 of them, e.g. 92541.
  - `view::Armor(npcRec).Enchantment()` returned a value for 4,083 of them.
- **Root cause:** paths are member indices, and nothing checks them against the record's type.
  - `Record::Get/Set/Has/Clear(FieldRef)`
    ([Record.h:177](../include/ESPER/Record.h)) ignore `FieldRef::record`.
  - Generated view constructors (`schemagen.py:1143`, `explicit {cls}(::esper::Record& r)`)
    don't compare `r.Sig()` with `kSig`.
  - `FromValue` converts any `Value` to any `T`: a string value read as `FormId` becomes 0.
- **Fix:** both halves removed.
  - **Views** (934d5fa): the `explicit View(Record&)` constructor is gone. `view::As<View>(rec)` returns
    `nullopt` for another record type. `esper-loadorder-test` checks that a KYWD view of an ARMO
    record is refused.
  - **Handles:** `Fields.h` and the `Record::Get/Set/...(FieldRef)` overloads were dropped. Views
    are the one typed API.

### `Plugin::Load` leaves `filename` empty, so self-references get an empty plugin name — FIXED
- **Symptom:** after `Load(path)`, `p.filename` is `""`. `FormIdToKey` on a self-reference
  then returns `FormKey{"", id}`, which renders as `000014:`. Every consumer has to remember
  `p.filename = …` after `Load`; SCT's sct-esm and ESPER's own tools all do. Reproduced
  2026-10-10 on Skyrim.esm.
- **Root cause:** [Plugin.cpp:224](../cpp/Plugin.cpp) `Plugin::Load` resets state and never
  derives `filename` from `path`. [Plugin.cpp:125](../cpp/Plugin.cpp) uses `filename` for every
  FormID at or past the master count.
- **Fix:** `Load` and `OpenHeader` set `filename` from the path (934d5fa). Callers that
  load under a different name can still override it afterwards. `esper-loadorder-test` checks
  it: it reloads a saved plugin without setting `filename`.

### STAT DNAM FormID offset pointed at a float — FIXED
- **Symptom:** a FormID remap over STAT records (`EnsureMaster` renumbering) treated the
  `maxAngle` float's bits as a FormID, which could rewrite the float. It never visited the real
  MATO reference, which kept pointing at the old master index.
- **Root cause:** `kStatDataIds` listed `FormIdAt(0)`. `StatData` is `{ float maxAngle; uint32_t
  material; }`, so the FormID is at offset 4
  ([RecordsWorld.h:115](../include/ESPER/RecordsWorld.h)).
- **Found by:** the migration step 2 converter, which turns FormID offsets into typed `formid`
  fields and reported that offset 0 lands on a float.
- **Verified:** with the schema declaring `material` as `formid` `to: [MATO]`, the schema gate
  resolves all 2,496 non-null vanilla STAT `material` values to MATO records (0 wrong).
- **Fix:** `FormIdAt(4)`, in the step 2 change.

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
