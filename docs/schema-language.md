# ESPER schema language — specification

**Language version 0.3.** This document is normative. It defines what a descriptor in `schema/`
means, so that any implementation can decode and encode Skyrim SE/AE plugin records exactly as
ESPER's engine does. When this document and the code disagree, that is a bug in one of them.
`tools/schemagen` is the reference validator, and `esper::Record` is the reference engine. The
design rationale is in [schema-design.md](schema-design.md).

## 1. Files

A schema is a directory tree of YAML files (`*.yaml` / `*.yml`). Each file holds one or more
YAML documents, and each document is one descriptor. The descriptor's kind is set by which of
these keys it has (exactly one):

| Key | Descriptor |
|---|---|
| `manifest` | schema metadata; exactly one per schema: `schemaVersion: "x.y.z"` |
| `record` | a record type (§5) |
| `struct` | a named, reusable byte layout (§4) |
| `group` | a named, reusable subrecord group (§6.3) |
| `table` | a lookup table that unions select on (§4.4) |

`enum` is reserved for a later version (§10).

Unknown keys are errors. A key that is reserved for a later version is rejected by name.

## 2. Names and signatures

- **Signatures** are exactly four printable ASCII characters (`EDID`, `NPC_`). Quote any
  signature YAML would read as a number or boolean.
- **Member and field names** are camelCase: `[a-z][A-Za-z0-9]*`. `self`, `record` and `cursor`
  are reserved.
- **Table names** are camelCase.
- **Record, struct and reusable-group names** are PascalCase: `[A-Z][A-Za-z0-9]*`. They name the
  generated view types, so they must be unique across the schema. Inline groups and inline
  structs are named by concatenating their owner's name with the PascalCased member name, for
  example `ArmorData`.

## 3. Types

A value (a member, a struct field, or a union case) is described by exactly one of:

- **`type:`** a base type, optionally followed by an array suffix (below);
- **`fields:`** an inline struct (§4);
- **`union:`** a layout chosen by data (§4.2).

`if:` (§4.3) and `remapUnsafe: true` (§8) can be added to any of them.

### 3.1 Base types

| Type | Encoding (little-endian) | Options |
|---|---|---|
| `u8 u16 u32 u64` | unsigned integer of that width | |
| `i8 i16 i32 i64` | two's-complement integer | |
| `f32` | IEEE-754 single; carried as raw bits, so every bit pattern round-trips | |
| `formid` | u32 FormID in the owning plugin's on-disk space | `to: [SIG, …]` (§8) |
| `sig4` | four bytes, a signature | |
| `zstring` | bytes followed by one NUL | |
| `wstring` | u16 byte length, then that many bytes | |
| `lstring` | in a **localized** plugin: u32 string id. Otherwise: as `zstring` | `table: strings \| dlstrings \| ilstrings` (required) |
| `bytes` | opaque bytes: exactly `size` if given, otherwise the rest of the subrecord | `size: N`, `formIds: none` (§8) |
| `marker` | zero bytes: the subrecord's presence is its value. A non-empty payload is refused. | |
| *StructName* | the named struct's layout (§4) | |

### 3.2 Array suffixes

| Suffix | Meaning |
|---|---|
| `T[]` | elements to the end of the subrecord |
| `T[N]` | exactly N elements |
| `T[count: u8]`, `T[count: u16]`, `T[count: u32]` | an element count of that width, then the elements |

The member's options (for example `to:` on `"formid[]"`) apply to the element type. An element
that consumes zero bytes is an error at decode time. Without that rule, a `T[]` array would loop
forever.

## 4. Structs

```yaml
struct: ObjectBounds
size: 12            # optional cross-check
fields:
  - { name: x1, type: i16 }
  - { break: true } # optional; see 4.1
  - { name: x2, type: i16 }
```

- Fields are laid out in order, with no implicit padding.
- **No byte is ever dropped.** Padding and unknown bytes must be named fields.
- `size`, when present, must equal the sum of the field sizes. A struct whose size varies
  (strings, arrays without a fixed length, breaks) can't declare a size.
- Only the **last** field may consume "the rest of the subrecord" (`T[]`, `bytes` without `size`).
  A struct can't contain itself.
- A member or field may define an inline struct with `fields:` instead of `type:`.
- A named struct may be a union instead: `struct: Name` with `union:` in place of `fields:`.

### 4.1 Breaks

`{ break: true }` marks a point where the payload may legitimately end: an older, shorter form of
the same struct.

- A struct with a break may only be a subrecord's **top-level** type. It can't be nested in
  another struct or used as an array element.
- A break can't follow a field that consumes the rest of the subrecord.
- **Decoding:** reaching a break with the payload exhausted ends the struct there. The value
  records how many fields are present. Reaching a break with bytes remaining continues parsing.
- **Encoding:** a value with fewer fields present is written up to its break, which reproduces
  the short form. Setting a field past the break extends the value to the full form, and the
  fields in between that have no value take their zero value.

### 4.2 Unions

A union picks one of several layouts at decode time.

```yaml
  - name: value
    union:
      by: { field: type }            # the selector
      cases:
        - { when: 1, name: object, type: VmadObject }
        - { when: [3, 4], name: num, type: i32 }
        - { when: { min: 11, max: 15 }, name: array, type: "u8[count: u32]" }
      default: { name: other, type: bytes }   # optional
```

**Selectors** (`by:`):

| Selector | Chooses the case by |
|---|---|
| `size` | the number of bytes left in the subrecord at the union's position |
| `formVersion` | the record header's form version |
| `record` | the enclosing record's signature; cases are written as signatures |
| `{ field: name }` | the value of an earlier field |
| `{ field: name, mask: N }` | that value ANDed with `N` |
| `{ lookup: { table, key, column } }` | the `column` value of the `table` row whose key equals the earlier field `key`; cases are written as column values |

- **Field references** resolve to the **nearest earlier field** with that name in the enclosing
  structs, searching outward. A union can therefore select on a field of a struct that contains
  it, such as VMAD's `objFormat`. Fields are looked up within the same subrecord only. A name that
  isn't a field anywhere in the schema is rejected.
- **Cases** match `when:` a value, a list of values, or `{ min, max }` (inclusive; either bound
  may be omitted). Cases are tried in order, then `default`.
  - A case without a payload (no `type`/`fields`/`union`) matches and consumes nothing.
  - A lookup key missing from the table matches no case and falls to `default`.
- **Refusal:** if no case matches and there is no `default`, the subrecord is refused (§7.1).
- **`name`** on a case is optional and names its generated accessor.
- **Value model:** a union's value records which case was taken. Its payload is that case's
  value. Paths (handles) step through a union by **case index**.

### 4.3 Conditional fields

`if:` makes a value present only when an earlier field satisfies a condition:

```yaml
  - { name: status, type: u8, if: { field: version, min: 4 } }
```

The condition is `{ field, [mask], min/max | equals | in: [..] }`. It is shorthand for a union
selecting on that field: one case with the value, and an empty default. Handles and views
address the value directly.

### 4.4 Tables

```yaml
table: ctdaFunctions
key: index
columns: [index, name, param1, param2]
rows:
  - [0, GetWantBlocking, none, none]
  - [1, GetDistance, form, none]
```

- The `key` column holds unique integers.
- Other columns hold values that `lookup` unions match by their written form.
- Tables appear in `schema.json`.

## 5. Records and members

```yaml
record: ARMO
name: Armor
members:
  - { sig: EDID, name: editorId, type: zstring }
  - { sig: KSIZ, name: keywordCount, type: u32, countOf: keywords }
  - { sig: KWDA, name: keywords, type: "formid[]", to: [KYWD] }
  - { sig: MODL, name: armatures, type: formid, repeat: true, to: [ARMA] }
  - { use: Model, name: maleModel, sigs: { MODL: MOD2, MODT: MO2T, MODS: MO2S } }
```

A record's `members` form its **root scope**. A member is either a **subrecord member** (`sig`
with `type` or `fields`) or a **group member** (§6).

| Key | Meaning |
|---|---|
| `repeat: true` | the member may occur any number of times. Without it, the member is **singular**. |
| `optional: true` | informational only (documentation, `schema.json`). Every member may be absent. |
| `countOf: <member>` | a **derived count** of a sibling member in the same scope, which must be repeated or array-typed. The counting member must be `u8`/`u16`/`u32`. See §7.3. |

A signature may appear only once among the **entry signatures** of a scope (§6.2). Otherwise
the descriptor is ambiguous and the generator rejects it.

## 6. Groups

A group is a run of subrecords that forms one logical item, such as a tint layer or a model slot.
A group is also a **scope**: inside it, signatures can mean something other than they do outside.

### 6.1 Inline groups

```yaml
  - group: tintLayers
    repeat: true
    members:
      - { sig: TINI, name: index,   type: u16, optional: true }
      - { sig: TINT, name: texture, type: zstring }
      - group: presets
        repeat: true
        members:
          - { sig: TINC, name: color, type: formid }
          - { sig: TINV, name: value, type: f32 }
```

### 6.2 Entry signatures and the ordinal rule

The **entry signatures** of a scope are the signatures of its subrecord members, plus, for each
group member, that group's own entry signatures. So a group is entered by any of its members,
including through nested groups. Entry signatures must be unique within each scope.

The root scope is **unordered**: its members may appear in any order. Group scopes are
**ordered** by the **ordinal rule**: an instance remembers the position of the last member it
placed, and a member at or before that position means the instance is complete. The exact
algorithm is in §7.1.

### 6.3 Reusable groups and signature remapping

```yaml
# schema/groups/Model.yaml
group: Model
members:
  - { sig: MODL, name: path, type: zstring }
  - { sig: MODT, name: data, type: bytes, formIds: none }
```

`{ use: Model, name: <member>, sigs: { MODL: MOD2, … } }` instantiates the group as a group
member.

- **`sigs`** maps the group's signatures to the ones used at this site. Each key must be a
  signature the group uses.
- **Nested reuse:** the inner remap applies first, then any remap from an enclosing `use`.
- **Allowed keys:** a `use` member may carry `repeat` and `optional`.
- **Validation:** remapping happens before validation, so the ambiguity rule sees the real
  signatures.

## 7. Semantics

### 7.1 Decoding a record

The payload is read as a sequence of subrecords. An `XXXX` subrecord gives the size of the one
that follows it; its original framing is remembered. A malformed subrecord stream fails the
decode.

The decoder keeps a stack of open scopes, starting with the root. Each open scope has a
*last position*, initially none. For each subrecord *s*:

1. **Find a scope.** Search the open scopes from innermost to outermost for one whose entry
   signatures contain *s*'s signature. If none has it, *s* is **raw** and belongs to the
   innermost open scope.
2. **Check it may go there.** Let *m* be the member it selects in that scope.
   - **Ordered scope:** *s* is acceptable if *m*'s position is after the last position, or equal
     to it when *m* is `repeat`. If not, continue the search outward from the next scope.
   - **Root scope:** if *m* is singular and already has an occurrence, *s* is **raw**.
3. **Accept it.**
   - Close every scope inside the accepting one.
   - **If *m* is a group member:** open a new instance of that group in the accepting scope,
     set the accepting scope's last position to *m*, push the instance, and resolve *s* inside
     it from step 1.
   - **If *m* is a subrecord member:** parse the payload with *m*'s type. On success the
     payload must be consumed exactly, so a layout that doesn't account for every byte refuses.
     The subrecord is then bound to *m*, and the scope's last position becomes *m*. On failure,
     *s* is **raw**, stays in place, and doesn't advance the position.

Refusal is normal, not an error. Raw subrecords keep their bytes and their position.

### 7.2 Encoding a record

Subrecords are written in order.

- **Raw and unmodified subrecords** are written from their original bytes, with their original
  framing.
- **Modified or new subrecords** are written from their value, keeping the original `XXXX`
  framing where there was one. `XXXX` is always used when a payload exceeds 65,535 bytes.

An implementation that verifies its layouts must also support a **forced** encode, which writes
every bound subrecord from its value. For every record of the vanilla masters, this must
reproduce the original bytes. It is ESPER's real-data gate.

### 7.3 Editing

- **No-op sets:** setting a value equal to the current one doesn't modify anything.
- **Where new subrecords go:** a new subrecord is placed in **schema order** within its scope
  instance. It goes after the last subrecord whose member position is the same or earlier, and
  otherwise before the first one that is later. A raw subrecord counts as earlier than every
  member. A new group instance is placed after the existing instances of the same group.
- **Derived counts:** when a member counted by a `countOf` sibling changes, the count is
  rewritten. It is set to the number of occurrences for a repeated member, the element count for
  an array, or the byte count for `bytes`. When the counted member becomes absent, the count
  subrecord is removed.
- **Unions:** setting a value inside a union case switches the union to that case, with zeros
  for any other fields of the case. Changing the field a selector reads does **not** re-choose
  cases in values that were already decoded. Keeping a value consistent with its selectors is
  the editor's job.
- **Ambiguous data:** the format itself can be ambiguous. For example, two instances of a
  repeating group with no member in common read back as one instance. Encoding writes what it
  is given; it doesn't disambiguate.

## 8. FormIDs

- Only values of type `formid` are FormIDs. FormIDs are in the owning plugin's on-disk space:
  the top byte indexes `[masters…, self]`.
- **`to: [SIG, …]`** declares which record types a FormID may reference. It is used for
  validation, typed links in generated views, and tools. ESPER's gate checks every declared
  target against the vanilla masters.
- **Remap safety.** A record is **remap-safe** only when every FormID it could contain is
  visible to the schema. A record is not remap-safe if it has any of these:
  - a raw subrecord;
  - a non-empty `bytes` value whose type doesn't declare `formIds: none`;
  - a value whose type is marked **`remapUnsafe: true`**. This is for words that might be FormIDs
    but can't be classified, such as a condition parameter whose function isn't in the table.

  An implementation must refuse to renumber FormIDs in a record that isn't remap-safe, and must
  leave it untouched. `formIds: none` is a claim about the data, verified on real records; it is
  never a default.

## 9. Outputs

`tools/schemagen` validates a schema and emits these outputs. They are committed to the repo, so
consumers never run the generator.

| Output | Content |
|---|---|
| `schema.json` | The resolved schema for data consumers: `schemaVersion`, `languageVersion`, and `records[]` with `sig`, `name` and `members`. Every member has its `name` plus either a `group` (view type name and nested `members`) or a `sig` and a fully resolved `type` (`kind`, and `fields`, `element`, `count`, `countPrefix`, `table`, `size`, `formIds`, `to` as applicable), and optionally `repeat`, `optional` and `countOf`. A union type is `{ kind: union, by, cases: [{ when \| default, name?, type? }] }`. `remapUnsafe` appears where set. `tables` holds every table. Reusable groups and named structs appear expanded at each use. |
| `include/ESPER/generated/Tables.h` | the engine's constexpr tables (`esper::schema::SchemaSet`) |
| `include/ESPER/generated/Fields.h` | typed handles: `esper::fields::<SIG>::<member>[::<field>]`; composite members expose `::self` |
| `include/ESPER/generated/Views.h` | typed views (`esper::view::<Name>`): accessors only, over the generic engine |

## 10. Reserved for later versions

These constructs come from the design in [schema-design.md](schema-design.md). The current
language version rejects them by name.

| Construct | Purpose |
|---|---|
| `open:` / `end:` on groups | explicit marker-delimited instances |
| `T[count: <field>]` | an array counted by an earlier field |
| `enum:`, `flags:`, `unit:` and `enum` descriptors | rendering hints |

## 11. Versioning

- `schemaVersion` (in the manifest) follows semver for the schema's content. Adding records,
  members or cases is a minor change. Renaming or removing a member, or changing a layout, is a
  major change.
- `languageVersion` is this document's version. It changes only when a construct is added or its
  meaning changes.

| Version | Change |
|---|---|
| 0.1 | Initial: scalars, strings, bytes, structs with breaks, arrays, groups (ordinal rule, reuse with remapping), `countOf`, `to:`, `formIds: none` |
| 0.2 | adds `marker` |
| 0.3 | adds unions (`size`, `formVersion`, `record`, `field`, `field` + `mask` and `lookup` selectors), `if:`, `remapUnsafe`, `table` descriptors, and named union structs |
