# ESPER — schema-first design

Status: **steps 1–3 and 5 implemented, step 4 partly done** (2026-10-09). See Migration plan for progress; the normative spec is
[schema-language.md](schema-language.md).

## What ESPER is

ESPER is a clean-room, **language-neutral schema of the Skyrim SE/AE plugin format**. The C++ library
in the same repo is the schema's reference implementation and the thing that verifies it.

| Layer | What it is | Who uses it |
|---|---|---|
| **Schema** | the descriptor-language spec, the `schema/` YAML descriptors, and a generated `schema.json` bundle | anyone, in any language; this is the primary product |
| **Generated C++** | field handles, typed views and schema tables, generated from the schema by ESPER's generator and committed to the repo | the engine and C++ consumers; consumers compile them like any other source and never touch YAML |
| **`esper::esper`** | the engine: plugin container (GRUP tree, compression, FormKeys, STRINGS), record decode/encode/visit, writer and master remap | C++ consumers; it also runs the gate that proves the schema |
| **`esper::loadorder`** (optional) | `esper::LoadOrderDocument`: resolve, load and query a whole load order | tools that want "the game's data as the game sees it" without writing that plumbing |

**Why one repo, not a schema repo plus an engine repo:** a schema change and the real-data run that
proves it must land in the same commit, or the two drift apart. The engine is how the schema stays
verified. ESPER is clean-room: its layouts are never checked against xEdit or Mutagen, so this gate
is the schema's only check. (Mutagen's *mechanism* was studied; see Prior art. Its definitions were not.)

```
schema/**/*.yaml ──► tools/schemagen ──┬─► schema.json                          (data consumers)
   (authored)        (Python, PyYAML)  ├─► include/ESPER/generated/Fields.h      (public handles)
                                       ├─► include/ESPER/generated/Views.h       (public typed views)
                                       └─► include/ESPER/generated/Tables.h      (engine tables)
                                                        │
                       esper::esper (engine) ◄──────────┘
                              │
                       esper::loadorder (LoadOrderDocument)
```

## Repo layout

```
schema/
  records/ARMO.yaml          one file per record type
  structs/ObjectBounds.yaml  reusable byte layouts
  groups/Model.yaml          reusable subrecord groups (applied with sig remapping)
  enums/*.yaml               enum/flag names (rendering hints; no effect on bytes)
  tables/ctda-functions.yaml lookup tables that unions reference
schema.json                  generated: the whole schema, resolved, in one file
docs/schema-language.md      the spec (normative): what a descriptor means
tools/schemagen/             the generator + validator (Python 3, PyYAML)
include/ESPER.h              umbrella (engine)
include/ESPER/LoadOrder.h    umbrella (optional load-order layer)
include/ESPER/generated/     generated, committed
cpp/                         engine + load-order implementation
```

## How it's consumed

| Mode | How | Gets |
|---|---|---|
| Data only | FetchContent with `SOURCE_SUBDIR` pointed at an empty dir, or a plain clone; read `schema.json` or `schema/` | the format, nothing compiled |
| Engine | link `esper::esper`, `#include <ESPER.h>` | decode/encode/write any modeled record |
| Engine + load order | also link `esper::loadorder`, `#include <ESPER/LoadOrder.h>` | `LoadOrderDocument` |

Both targets build by default; they're plain C++ with no extra dependencies, so a consumer that
doesn't link `esper::loadorder` simply ignores it. `ESPER_BUILD_LOADORDER=OFF` skips building it.

## The descriptor language

`docs/schema-language.md` will be the normative spec. This section is its outline.

### File kinds

`record`, `struct`, `group`, `enum`, `table`. Records reference the others by name. The
generator resolves every reference and rejects anything dangling.

### Types

| Type | Bytes |
|---|---|
| `u8 u16 u32 u64 i8 i16 i32 i64 f32` | little-endian scalars |
| `formid` (optional `to: [ARMO, ARMA]`) | u32; the **only** type FormID visitation and remap touch. `to:` lists the record types it may point to, for validation, typed links in views, and UIs |
| `sig4` | a 4-character tag stored as u32 |
| `zstring` | NUL-terminated; refuses embedded NULs or a missing terminator |
| `wstring` | u16 length + bytes (VMAD) |
| `lstring` (`table: strings / dlstrings / ilstrings`) | u32 string id if the plugin is localized, else a zstring |
| `bytes` (`size: N`, or the rest of the subrecord) | opaque, kept exactly; remap-unsafe when non-empty unless declared `formIds: none` |
| `T[]` | array to the end of the subrecord |
| `T[N]` | fixed-length array |
| `T[count: u32]` | array prefixed by its count |
| `T[count: <field>]` | array whose count is an earlier field |

### Structs

```yaml
struct: ObjectBounds
size: 12                  # cross-check: field sizes must sum to this
fields:
  - { name: x1, type: i16 }
  - { name: y1, type: i16 }
  - { name: z1, type: i16 }
  - { name: x2, type: i16 }
  - { name: y2, type: i16 }
  - { name: z2, type: i16 }
```

**No byte is ever dropped.** Padding and unknown bytes are named fields (`{ name: unknown0, type:
bytes, size: 3 }`). A layout must account for every byte of the payload, or the subrecord is refused.

**Truncated forms: `break`.** Most size variants in the format are an older, shorter form of the
same struct. A `break` marks a point where the payload may legitimately end:

```yaml
fields:
  - { name: flags,   type: u32 }
  - { name: radius,  type: f32 }
  - { break: true }               # older form ends here
  - { name: falloff, type: f32 }
```

- **Decode:** the payload must end exactly at a break or at the full size; anything else is refused.
  The node records which break it ended at.
- **Encode:** the fields are written up to that break, so the short form is reproduced exactly.
- **Setting a field past the break** extends the struct to the full form. Fields in between that have
  no value become zero.
- **Unions by `size`** remain for variants that aren't plain truncations.

### Records and members

```yaml
record: ARMO
name: Armor
members:
  - { sig: EDID, name: editorId, type: zstring }
  - { sig: VMAD, name: scripts,  type: Vmad }
  - { sig: OBND, name: bounds,   type: ObjectBounds }
  - { sig: FULL, name: fullName, type: lstring, table: strings }
  - { sig: KSIZ, name: keywordCount, type: u32, countOf: keywords }
  - { sig: KWDA, name: keywords, type: "formid[]" }
  - { sig: MODL, name: armatures, type: formid, repeat: true }
  - sig: DATA
    name: data
    fields:
      - { name: value,  type: u32 }
      - { name: weight, type: f32 }
```

- Members are **singular** by default. A second occurrence of a singular member is refused and stays
  raw. `repeat: true` allows any number.
- `countOf` marks a **derived** field. When the counted member is modified, the engine recomputes the
  count. An unmodified record keeps its original bytes, so it is never touched.

### Groups

A group is a run of subrecords that forms one logical item. Groups are also how scoping works.

```yaml
  - group: tintLayers
    repeat: true
    members:                       # ordered: an instance fills these in order
      - { sig: TINI, name: index,    type: u16 }
      - { sig: TINT, name: texture,  type: zstring }
      - { sig: TINP, name: maskType, type: u16,    optional: true }
      - { sig: TIND, name: default,  type: formid, optional: true }
      - group: presets
        repeat: true
        members:
          - { sig: TINC, name: color, type: formid }
          - { sig: TINV, name: value, type: f32 }
          - { sig: TIRS, name: id,    type: u16 }
```

Instance boundaries follow the **ordinal rule.** The members are ordered, and the decoder remembers
the last member position filled in the current instance. When a sig arrives:

- **It matches a member after that position,** or the same position for a `repeat` member: it goes
  into the current instance.
- **It matches a member at or before that position:** the sequence has restarted. The instance
  closes; if the group repeats, a new instance opens with this sig.
- **The group can't place it:** the instance closes and the sig is resolved in the enclosing scope.

So any member can be optional, including the first. There's no fixed opener to depend on. Explicit
delimiters are still available where the data has them:

- **`open: NAM0`:** a marker sig starts each instance.
- **`end: ALED`:** a sig ends it.

**Sig resolution:** a sig is resolved against the innermost open group first, then outward. That
lets RACE reuse `MODL`/`ANAM` in different sections. The generator rejects any scope where one sig
could bind two ways.

**Reusable groups and sig remapping.** A group defined once can be applied under different
signatures. This replaces copy-pasted runs like ARMA's four model slots:

```yaml
# schema/groups/Model.yaml
group: Model
members:
  - { sig: MODL, name: path,          type: zstring }
  - { sig: MODT, name: textureHashes, type: bytes, formIds: none }
  - { sig: MODS, name: swaps,         type: bytes }

# in a record:
  - { use: Model, name: maleModel,   sigs: { MODL: MOD2, MODT: MO2T, MODS: MO2S } }
  - { use: Model, name: femaleModel, sigs: { MODL: MOD4, MODT: MO4T, MODS: MO4S } }
```

The generator applies the remap before validation, so ambiguity checks see the real sigs.

### Unions

A union picks a layout from a closed set of **selectors**. There are no host-language hooks.

| Selector | Picks the case by |
|---|---|
| `size` | the payload size |
| `formVersion` | the record header's form version |
| `record` | the enclosing record's sig (e.g. VMAD's per-record fragment trailer) |
| `field: <name>` | the value of an earlier field; the name resolves to the nearest one in the enclosing structs, so it can reach up a level (VMAD `objFormat`) |
| `field: <name>, mask: <bits>` | that value ANDed with a mask (flag bits) |
| `lookup: { table, key, column }` | a column of a schema table, keyed by a field (CTDA param kinds by function index) |

- Cases match `when: <value>` (or a list of values). `default:` covers anything else.
- No matching case and no default means the subrecord is refused.
- A **conditional field** is shorthand for a union with an empty case:
  `{ name: status, type: u8, if: { field: version, min: 4 } }`.

**Worked example: VMAD,** from ESPER's current codec. It needs no C++:

```yaml
struct: Vmad
fields:
  - { name: version,   type: i16 }
  - { name: objFormat, type: i16 }
  - { name: scripts,   type: "VmadScript[count: u16]" }
  - name: fragments                 # QUST/INFO/PACK/PERK/SCEN trailers, modeled later
    union: { by: record, cases: [], default: { type: bytes } }

struct: VmadScript
fields:
  - { name: name,       type: wstring }
  - { name: status,     type: u8, if: { field: version, min: 4 } }
  - { name: properties, type: "VmadProperty[count: u16]" }

struct: VmadProperty
fields:
  - { name: name,   type: wstring }
  - { name: type,   type: u8 }
  - { name: status, type: u8, if: { field: version, min: 4 } }
  - name: value
    union:
      by: { field: type }
      cases:
        - { when: 1,  type: VmadObject }
        - { when: 2,  type: wstring }
        - { when: 3,  type: i32 }
        - { when: 4,  type: f32 }
        - { when: 5,  type: u8 }
        - { when: 11, type: "VmadObject[count: u32]" }
        # 12..15: arrays of the same, likewise

struct: VmadObject
union:
  by: { field: objFormat }
  cases:
    - when: 1
      fields: [ { name: formId, type: formid }, { name: alias, type: i16 }, { name: unused, type: u16 } ]
    - when: 2
      fields: [ { name: unused, type: u16 }, { name: alias, type: i16 }, { name: formId, type: formid } ]
```

**Worked example: CTDA parameters.** These are data-driven through the function table:

```yaml
  - name: param1
    union:
      by: { field: op, mask: 0x02 }            # "params use aliases" flag
      cases:
        - { when: 0x02, type: i32 }            # alias index, never a FormID
      default:
        union:
          by: { lookup: { table: ctdaFunctions, key: functionIndex, column: param1Kind } }
          cases:
            - { when: form, type: formid }
            - { when: int,  type: i32 }
          default: { type: u32, remapUnsafe: true }   # unverified function: remap refuses
```

### Rendering hints

`enum:`, `flags:` and `unit:` change only how a value is displayed, never its bytes. They are
there for dumps, diffs and UIs.

### Naming

Field names are camelCase. Today's member names are kept (`editorId`, `fullName`, `keywords`)
so existing code changes as little as possible.

### Growth rule

If something genuinely can't be expressed, **the language grows**: the spec gets a new construct, and
the generator and engine implement it. Behavior that lives only in ESPER's C++ is never allowed,
because data consumers couldn't reproduce it.

## Semantics every implementation must honor

These go in the spec, because anyone else's engine has to behave the same way:

1. **Exact accounting.** A layout either consumes every byte of a subrecord or the subrecord is
   refused (kept raw). Refusal is normal, not an error.
2. **Scoped sig resolution and the ordinal rule**, as described under Groups.
3. **Verbatim unless modified.** Each subrecord keeps its original bytes. Encode emits them unless the
   subrecord (or a field it derives from) was modified.
4. **FormIDs.** Only `formid` is a FormID. 0 is null and is never remapped. A record refuses remap,
   rather than guessing, if it has any of these:
   - a raw subrecord;
   - a `remapUnsafe` value;
   - a non-empty `bytes` field that isn't declared `formIds: none`.

   Opaque bytes are therefore assumed to *might* hold FormIDs. Today's `Blob` binder assumes the
   opposite, and that is an open bug ([bugs.md](bugs.md)).
5. **Context.** Encoding `lstring` depends on the plugin's localized flag.
6. **Breaks.** A truncated struct is re-emitted at the break it ended on.

## Generated outputs

- **`schema.json`:** the whole schema, references resolved, plus `schemaVersion` and
  `languageVersion`. Any language can read it with the JSON support in its standard library.
- **`include/ESPER/generated/Fields.h`** (public): constexpr handles like `f::NPC_::race` and
  `f::RACE::tintLayers::texture`. Each handle carries its value type, so a wrong name or wrong type
  is a compile error.
- **`include/ESPER/generated/Views.h`** (public): typed views, described under Engine below.
- **`include/ESPER/generated/Tables.h`:** flat constexpr arrays (fields, structs, members, records,
  tables) linked by index. Included by the engine's .cpp files only.
- **The generator** (`tools/schemagen`, Python 3 + PyYAML) validates before emitting anything:
  - references resolve;
  - struct sizes add up;
  - no sig is ambiguous within a scope;
  - selectors only reference earlier fields.
- **Freshness:** output is committed. `esper-schemagen` regenerates it, and the CTest check
  `esper-schema-fresh` fails on stale output. Both exist only when Python and PyYAML are found.

## Engine

- **Dynamic record model.** A record is an ordered list of subrecord nodes. Each node holds:
  - its definition, or none if it's raw;
  - a span of its original bytes;
  - its decoded value tree, decoded on first access;
  - a dirty bit.

  A group instance is a range of nodes. Encode order is node order. A decoded value is cached on its
  node, so repeated access doesn't re-decode. (Mutagen's overlays re-decode on every access.)
- **Value kinds:** `int`, `uint`, `float`, `string`, `formid`, `lstring`, `bytes`, `struct`, `array`.
- **API conventions** (from the API pass, 2026-10-10):
  - **Errors:** every fallible call returns `esper::Result<T>`, which is
    `std::expected<T, esper::Error>`. There are no `char*` error buffers and no bool plus
    out-parameter pairs.
  - **Records come back by value:** `Record::Decode(plugin, node)` → `Result<Record>`, and
    `Record::Create(plugin, sig)` makes a new, empty record. `Record::Commit` refuses a record of
    another type.
  - **Typed views are checked:** `view::As<View>(rec)` → `std::optional<View>`, `nullopt` for
    another record type. There's no public `View(Record&)` constructor.
  - **Plugins:** `Load(path)` names the plugin from the path. `Records(sig)` lists records in file
    order, and `Find(formId)` looks one up. `SavePlugin` finalizes a plugin edited through the
    mutation API.
  - **FormIDs** in callbacks are `FormId`. `FormKey::Parse` is the inverse of `ToString`.

  ```cpp
  #include <ESPER.h>

  esper::Plugin plugin;
  if (auto ok = plugin.Load(path); !ok) return ok.error();
  for (esper::RecordNode* node : plugin.Records(esper::Sig4("NPC_"))) {
      auto rec = esper::Record::Decode(plugin, *node);      // Result<Record>
      if (!rec) continue;
      auto npc = esper::view::As<esper::view::Npc>(*rec);   // checked
      npc->SetEditorId("Renamed");                          // marks the subrecord dirty
      esper::Record::Commit(plugin, *node, *rec);
  }
  esper::SavePlugin(plugin, outPath);                       // finalizes the edited plugin

  rec->Find("maleModel/path");                              // string paths for tools/UIs
  rec->VisitFormIds([](esper::FormId& id) { /* remap */ }); // refuses if unsafe
  ```

- **Typed views (generated, `Views.h`).** One small class per record, struct and group, wrapping a
  position in an `esper::Record` without owning it. Each accessor is a one-line call into the
  engine, so a view is ergonomics only: all serialization stays in the one interpreted engine.

  ```cpp
  auto d = doc.Decode(key);                                 // Result<DecodedRecord>
  auto npc = esper::view::As<esper::view::Npc>(d->record);
  std::string name = doc.Text(*d, *npc->FullName());        // the field's string table
  for (esper::view::RaceTintLayers layer : race->TintLayers())
      use(layer.Texture(), layer.Presets());
  const esper::ResolvedRecord* target = doc.Follow(*d->source, *armor->Enchantment());  // Link<View>
  ```

  - **Optional members** return `std::optional<T>`. Repeated members and groups return vectors.
  - **Unions** get a generated `<Acc>Kind` enum of their cases, `<Acc>Case()` (which case was
    decoded), `<Acc>Is<Case>()`, and an accessor per named case's payload.
  - **`formid` with `to:`** becomes `Link<View>`, which `LoadOrderDocument::Follow` resolves and
    checks against the target type. A `formid` without `to:` stays a plain `FormId`.
  - **Generated output** is accessors only, with no read or write code. That keeps it small,
    unlike Mutagen's ~1.4M generated lines for Skyrim.

- **Gates.**
  - `esper-test`: synthetic fixtures, run by CTest.
  - `roundtrip-gate <Data>`: real masters, with **forced re-encode**. Every modeled subrecord is
    re-encoded from its value tree, ignoring the verbatim shortcut, and the result must be
    byte-identical. This is how a codec bug is caught even though clean records would normally
    never be re-encoded.

## Load order: `esper::LoadOrderDocument`

This is the generic half of what Scene Editor v2's `LoadOrderDocument` does today through
sct-esm/SctEsmBackend. Any tool opens it and gets the game's data as the game would see it.

**In scope** (pure plugin-format knowledge):

- **Resolution:** implicit masters, `plugins.txt`, `Skyrim.ccc`, the ESM/ESL hoist, missing and
  disabled plugins. This is today's `LoadOrderReader`.
- **Loading:** every enabled plugin, the override chains, FormKey ↔ runtime FormID including the 0xFE
  light-plugin mapping. This is today's `LoadOrderIndex`.
- **Localized strings:** loaded per language, so `lstring` values resolve to text.
- **Lazy, overlay-style loading.** Opening a load order maps each plugin and builds a per-group index
  of FormKey → byte offset. Records are decoded only when something asks for them. Opening a full
  modded load order therefore costs a header scan, not a full decode.
- **Generic queries:**
  - `Plugins()`
  - `Resolve(FormKey, Winner | Origin)`: the winning override, or the record as first defined.
    Resolution is lazy, walking from the highest-priority plugin down and caching what it learns.
  - `Chain(FormKey)`: every override, in load order
  - `Enumerate(sig)`, over winning records
  - `Search(sig, text)`, by EditorID or resolved name
  - `Decode(FormKey)` → `esper::Record`, and `Decode<View>(FormKey)` → a typed view
  - `Resolve(Link<View>)` → the target's typed view
  - `WorldIndex`, for cells and worldspaces
- **Override into a plugin:** `OverrideInto(plugin, FormKey)` copies the winning record into a
  plugin being authored, adding masters as needed. It returns the editable record, or an existing
  override if the plugin already has one.

**Out of scope** (application semantics, which stay in the tool):

- NPC → worn armor → NIF resolution
- `ActorDocument`
- asset lookup in BSAs and loose files

Scene Editor v2's `LoadOrderDocument` becomes a thin layer on top of this one.

## Prior art: Mutagen's mechanism

Mutagen (C#) was studied for **mechanism only**. Its record definitions were not read or used.

**Borrowed:**

| Mechanism | Where it landed here |
|---|---|
| Implicit group boundaries from "last parsed position" | the ordinal rule under Groups |
| `Break` for truncated layouts | `break` under Structs |
| Signature remap tables for reused sub-objects | reusable groups + `sigs:` |
| Typed links to a set of record types | `formid` `to:` → `Link<View>` |
| Overlay: scan headers once, store offsets, decode on access; FormKey → offset per group | node spans + lazy decode; `LoadOrderDocument` lazy loading |
| Link cache with winner/origin resolution and "add as override" | `Resolve(…, Winner \| Origin)`, `OverrideInto` |
| Typed per-record API | generated typed views (accessors only) |

**Avoided, with Mutagen as the evidence:**

- **Host-language escape hatches.** Skyrim has 291 custom hooks in about 15k hand-written lines,
  often implemented three times (read, write, overlay). The hardest semantics live there, outside
  the schema. Here: the growth rule.
- **Lossy round-trip.** Mutagen drops unknown subrecords and trailing bytes by default, and its tests
  compare against a normalized copy of the input. Here: byte-exact, and refusal instead of dropping.
- **Per-record generated serialization.** Version and break arithmetic is duplicated across the
  generated read, write and overlay code (~1.4M lines for Skyrim). Here: one interpreted engine.
- **Grammar split across three places:** an advisory XSD, the generator code, and enums in C#.
  Here: the spec is normative and the generator validates against it.
- **Hard generator limits,** such as two versioning entries per field and no third occurrence of a
  repeated sig. Here: the general selectors.

## Versioning

- `schemaVersion` is semver. Adding records, fields or union cases is a minor bump. Renaming or
  removing a field, or changing a layout, is a major bump.
- `languageVersion` is versioned separately and bumps only when the spec gains or changes a construct.
- Tags on the repo carry the schema version. The engine's API follows it until there's a reason to
  version them apart.

## Migration plan

Each step ships on its own. The typed `Records*.h` path keeps working until step 7.

1. **Done (2026-10-09). Spec, generator, engine skeleton.** Write `docs/schema-language.md` v0, and the generator with
   validation and all four outputs (`schema.json`, handles, views, tables). Build the engine for
   scalars, structs (including `break`), members and arrays. Bring five simple records (KYWD, GLOB,
   TXST, MISC, ARMO) through it, with unit tests. ARMO exercises reusable groups and sig remapping.

   Result on the five vanilla masters (`roundtrip-gate --schema`):
   - 7,943 records decode, and re-encode byte-exactly both verbatim and forced;
   - raw subrecords exactly match the typed path's unknowns;
   - FormID sets are identical on all 7,209 records both paths can remap;
   - all 31,876 `to:` link targets are verified.

   The new path refuses remap on 724 records the typed path accepted: opaque `VMAD` (language 0.2)
   and `MODS`/`MO2S`/`MO4S`, which the typed path assumed were FormID-free (see bugs.md).
2. **Done (2026-10-09). Seed the descriptors.** A one-shot script (not kept in the repo) converted
   all of `Records*.h` into YAML: 880 rows into 115 records plus 56 structs, on top of the five
   step-1 records. Along the way it:
   - turned the hand-listed FormID offsets into typed `formid` fields;
   - folded model slots into `use: Model`, and conditions into a reusable `Condition` group
     (CTDA kept opaque until lookup unions; CIS1/CIS2 now bound);
   - added `marker` (language 0.2);
   - found the STAT `DNAM` FormID-offset bug ([bugs.md](bugs.md)).

   Hand fixes: `CLFM` named `ColorForm` (its `color` member would have collided with a `Color`
   view), and STAT `material` declared `to: [MATO]` so the gate verifies the fix.
3. **Done (2026-10-09), as `roundtrip-gate --schema`. Parity oracle.** It runs both paths over
   every vanilla record and requires:
   - byte-exact decode → encode (verbatim and forced);
   - no more raw subrecords than the typed path's unknowns (by count per record);
   - identical FormID multisets wherever both paths can remap;
   - verified `to:` targets.

   Result: all 1,178,543 vanilla records (5.79M subrecords) pass. 81.85% of subrecords are
   modeled, and the new path keeps 10,026 fewer raw than the typed path. There are 0 FormID
   mismatches across 945,927 records. The new path refuses remap on 98,384 records the typed path
   accepted, because they carry opaque bytes (quantified in bugs.md).
4. **Groups (ordinal rule), unions, VMAD, CTDA table.** This moves the RACE, QUST, NPC_ CSDT and size-variant cases
   out of passthrough, and lets records with conditions be remapped. Each case is gated on real data.

   **Done (2026-10-09): everything the typed path already knew.**
   - Language 0.3: unions with all six selectors, `if:`, `remapUnsafe`, `table` descriptors.
   - CTDA typed via `tables/ctdaFunctions` (401 functions converted from `CtdaFunctions.cpp`).
   - VMAD modeled in full except the per-record fragment trailers.
   - CELL `DATA` u8/u16 variants.
   - RACE and NPC_ tint layers as nested groups. Structure checked on `Skyrim.esm`: 1,445 RACE
     layers for 1,445 `TINI`, and 33,050 complete NPC_ layers.

   Gate result:
   - remap refusals fell from 98,384 to 68,299;
   - FormID parity holds on 976,012 records (0 mismatches);
   - 87,563 typed links are verified, including GLOB comparison values and CLFM tint colors.

   **Remaining, which needs new format research** because the typed path never modeled it:
   - QUST alias, stage and objective sections (needs `open:`/`end:` markers);
   - NPC_ `CSDT`, PERK sections, PACK data trees, IMAD keyframes;
   - VMAD fragment trailers;
   - the size-variant blobs: `XCLL`, WRLD `MNAM`, EXPL/PROJ `DATA`, ECZN `DATA`, FACT
     `CRVA`/`PLVD`;
   - the largest opaque blockers: `MHDT`, `NVNM`, `TVDT`.

   **Research round 1 (2026-10-09).** Layouts were derived from vanilla data with `schema-probe`,
   which profiles each subrecord column by column. Modeled:
   - every REFR, ACHR and CELL subrecord except `TVDT`;
   - MGEF `DATA` (14 FormID fields);
   - WEAP `DNAM`, BOOK `DATA` (a spell-or-skill union), FACT `CRVA` (three sizes), ECZN/RELA/SNDR
     data, `MODS` alternate textures, destruction data (a reusable `Destructible` group), and more.

   Gate result:
   - 85.76% of subrecords modeled (from 81.85%);
   - remap refusals 29,119 (from 68,299);
   - 104,539 records remap-safe that the typed path couldn't remap;
   - 201,287 typed links verified, 0 wrong;
   - FormID parity with 0 cases of the schema missing what the typed path saw. The 7,250
     FormIDs the typed path skipped are logged in bugs.md.

   Still opaque:
   - NAVM `NVNM` (19,440 records): navmesh geometry, a complex variable layout;
   - CELL `TVDT` (8,595): unexplained unaligned hits, so not provably FormID-free;
   - sectioned records QUST, SCEN, PACK and PERK, which need `open:`/`end:` group markers;
   - a long tail of small subrecords.

   **Sectioned records (2026-10-09).** Two language versions:
   - **0.4:** delimited groups (`open:`/`end:`);
   - **0.5:** the `member` selector, and `open`/`end` sharing one signature.

   With them, QUST, PERK, SCEN and PACK are modeled from vanilla evidence:
   - **QUST:** stages with nested log entries, objectives with targets, and reference and location
     aliases;
   - **PERK:** entries `PRKE` … `PRKF` with a `DATA` size union, condition tabs, and `EPFD` chosen
     by the sibling `EPFT`;
   - **SCEN:** phases `HNAM` … `HNAM` with completion blocks, actors, and actions `ANAM` … `ANAM`;
   - **PACK:** data inputs and procedure-tree branches opened by `ANAM`, input names opened by
     `UNAM`, and the begin/end/change event blocks.

   Every section count was checked against its marker count on the masters. Examples:
   - 7,826 QUST stages for 7,826 `INDX`;
   - 70,452 PACK sections for 70,452 `ANAM`;
   - every alias, PERK entry and SCEN action closed by its end marker;
   - each PACK input type lands on its own value member: Bool → `CNAM` u8, Float/Int → `CNAM`
     u32, Location → `PLDT`, SingleRef → `PTDA`, Topic → `PDTO`.

   Raw left: QUST 0 (typed path 131,700 unknown), PERK 0 (5,804), SCEN 6 (282,636), PACK 1
   (314,480).

   Gate result:
   - 98.32% of subrecords modeled (from 85.76%);
   - 114,643 records remap-safe that the typed path couldn't remap (from 104,539);
   - 252,304 typed links verified, 0 wrong;
   - remap refusals unchanged at 29,119.

   Still opaque: NAVM `NVNM`, CELL `TVDT`, INFO response data, and the long tail.
5. **Done (2026-10-09). `esper::loadorder`.** `LoadOrderDocument` wraps
   `LoadOrderReader`/`LoadOrderIndex`/`WorldIndex`. It adds:
   - `Resolve` (winner/origin) and `Follow` (FormIDs and typed links);
   - schema decode into `esper::Record` and typed views;
   - localized names and an EDID/FULL-only search;
   - a lazily built `WorldIndex`;
   - `SnapshotJson`;
   - `OverrideInto`, which adds masters in load order, rewrites every FormID into the target's
     space, and converts localized strings via `Record::Relocalize`. It refuses child records
     and records that aren't remap-safe.

   The "per-group FormKey → offset index" is the existing record index: `Plugin::Load` already
   scans headers only, with payloads as views into the file buffer that are inflated on demand.

   Result on the vanilla install (`loadorder-smoke`): 10 plugins load in 3.8 s. Lydia is found by
   her localized name and her race link resolves to NordRace. 55 vanilla records override into a
   new plugin, which saves and reloads clean; 45 are refused, all for opaque blobs. 49
   worldspaces index in 175 ms.
6. **API pass (2026-10-10): done except handles.** A usability review of the consumer surface
   found that SCT used none of the schema engine. It used the typed path plus hand-walked
   subrecords, and built its own facade over ESPER's gaps. Changes:
   - `Result<T>` errors everywhere;
   - checked views (`view::As`);
   - records by value;
   - `Plugin::Load` names the plugin (a bug: bugs.md);
   - `Plugin::Records`/`Find`;
   - `SavePlugin` auto-finalizes;
   - union case enums;
   - `LocalizedString` carries its table, so `doc.Text(decoded, value)` needs nothing else;
   - `FormKey::Parse`;
   - `FormId` in FormID callbacks;
   - `SigName`.

   **Open:** whether field handles (`Fields.h`) stay public. Nothing outside tests uses them, and
   they still skip the record-type check that views now have.
7. **Port SCT.** sct-esm and Scene Editor v2 move to typed views and `LoadOrderDocument`, and SCT's
   ESPER pin is bumped.
8. **Delete the typed world:** `Records*.h`, the `Field<>`/`MakeSchema` templates, the old
   `SchemaRegistry`, `CtdaFunctions.cpp` (now `tables/ctda-functions.yaml`), and the hand-written
   VMAD codec.

## Decisions

Settled 2026-10-08:

- **Schema-first.** The schema is the product; the engine is its reference implementation, in the
  same repo.
- **YAML descriptors.** A Python generator produces `schema.json` and C++ headers, which are
  committed. C++ consumers build from the generated headers and never touch YAML or Python.
- **Verbatim unless modified,** with a forced re-encode real-data gate.
- **Generated field handles,** plus string paths.
- **Clean-room.** Descriptors are written from real-data verification only. xEdit and Mutagen record
  definitions are never consulted.

Settled 2026-10-09:

- **Mutagen's mechanism studied** (mechanism only). The borrowings are listed under Prior art.
- **Generated typed views.** Accessors only, over the one interpreted engine.
- **Optional load-order layer.** `esper::LoadOrderDocument` lives in its own target.

Settled 2026-10-09 (defaults accepted):

- **Snapshot export:** yes, as a later addition. `LoadOrderDocument` will be able to write a JSON
  snapshot (plugin list, flags, masters) for out-of-process tools.
- **Load-order inputs:** ESPER finds the default `plugins.txt` location itself
  (`%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt`), and tools can pass their own paths
  instead (MO2 profiles, VFS).
- **Data bundle:** `schema.json` only. The YAML tree is the source, not a second bundle.
