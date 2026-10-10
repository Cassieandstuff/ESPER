# ESPER

A clean-room C++23 reader/writer for Bethesda plugin files (`.esm` / `.esp` / `.esl`),
targeting **Skyrim Special Edition / Anniversary Edition** (VR is format-identical).
Initially built for Skyrim Content Tools' Scene Editor.

- **Container** — whole-file parse into a structure-preserving GRUP tree with a full record
  index; FormKey identity; load-order resolution (`plugins.txt` + `Skyrim.ccc`) and override
  chains.
- **Records** — a declarative schema per record type; decode, encode and FormID visitation all
  derive from the one table. Unmodeled subrecords pass through verbatim, so every record
  round-trips byte-exact.
- **Write** — verbatim-unless-modified serialization plus a mutation API (create records,
  add masters, remap FormIDs, finalize).
- **World** — WRLD → (x, y) → CELL index, interiors, terrain enumeration.
- **Localization** — STRINGS / ILSTRINGS / DLSTRINGS sets.

No dependencies beyond the standard library (zlib comes from a vendored copy of miniz).

## Using it

Pull it in with CMake's FetchContent and link `esper::esper`:

```cmake
include(FetchContent)
FetchContent_Declare(ESPER
    GIT_REPOSITORY https://github.com/Cassieandstuff/ESPER.git
    GIT_TAG        main)   # pin a tag or commit for reproducible builds
FetchContent_MakeAvailable(ESPER)

target_link_libraries(MyPlugin PRIVATE esper::esper)
```

Then include the one header — typically from your precompiled header, next to
`<RE/Skyrim.h>`:

```cpp
#include <ESPER.h>

esper::Plugin plugin;
if (auto ok = plugin.Load(path); !ok) return fail(ok.error().message);

for (esper::RecordNode* node : plugin.Records(esper::Sig4("ARMO"))) {
    auto rec = esper::Record::Decode(plugin, *node);           // esper::Result<Record>
    if (!rec) continue;
    auto armor = esper::view::As<esper::view::Armor>(*rec);    // typed, checked view
    if (armor->ArmorRating().value_or(0) < 1000) armor->SetArmorRating(1000);
    esper::Record::Commit(plugin, *node, *rec);                // only the changed subrecord re-encodes
}
esper::SavePlugin(plugin, outPath);
```

Everything lives in `namespace esper`. Every call that can fail returns `esper::Result<T>`
(`std::expected<T, esper::Error>`).

ESPER does not set the MSVC runtime library; it inherits `CMAKE_MSVC_RUNTIME_LIBRARY` from
your project, so it links cleanly into both static-CRT SKSE plugins and dynamic-CRT desktop
tools. CMake 3.28+ and a C++23 compiler are required.

`esper::miniz` is also exported, so a consumer that needs zlib itself can link that copy
instead of compiling a second one.

### The load order (optional)

Link `esper::loadorder` and include `<ESPER/LoadOrder.h>` to work with a whole load order:

```cpp
esper::LoadOrderDocument doc;
esper::LoadOrderOptions opt;
opt.dataFolder = R"(C:\Steam\steamapps\common\Skyrim Special Edition\Data)";
// opt.pluginsTxt = a mod manager profile's plugins.txt (default: %LOCALAPPDATA%)
if (auto ok = doc.Load(opt); !ok) return fail(ok.error().message);

for (const esper::ResolvedRecord* r : doc.Search(esper::Sig4("NPC_"), "Lydia")) {
    auto d = doc.Decode(*r);                         // the winning override
    if (!d) continue;
    auto npc = esper::view::As<esper::view::Npc>(d->record);
    std::string name = doc.Text(*d, *npc->FullName());   // localized or inline
    const esper::ResolvedRecord* race = doc.Follow(*r, *npc->Race());
    std::printf("%s is a %s\n", name.c_str(), doc.Name(*race).c_str());
}

esper::Plugin mine;                                  // a plugin being authored
esper::CreateEmptyPlugin(mine, "MyMod.esp", false);
auto key = esper::FormKey::Parse("0A2C8E:Skyrim.esm");
doc.OverrideInto(mine, *key);                        // masters + FormIDs + strings handled
esper::SavePlugin(mine, outPath);
```

It resolves plugins.txt, Skyrim.ccc and the master hoist, and answers queries over the result:

- **Records:** override winners, origins and chains; FormID and typed-link following.
- **Text:** localized names, and EditorID/name search.
- **Worldspaces and cells:** `WorldIndex`.
- **Snapshot:** `SnapshotJson()` writes the resolved load order as JSON.

Vanilla string tables ship inside BSAs and ESPER doesn't read archives, so pass
`LoadOrderOptions::fetch` to resolve names from archives or from extracted files.

## Building standalone

```
cmake -S . -B build
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo
```

| Option | Default | |
|---|---|---|
| `ESPER_BUILD_TESTS` | ON when top-level | `esper-test`, the synthetic-fixture test suite (registered with CTest) |
| `ESPER_BUILD_LOADORDER` | ON | `esper::loadorder` (LoadOrderDocument) |
| `ESPER_BUILD_TOOLS` | OFF | `roundtrip-gate` (the real-data gate), `esm-dump` (a read-only record dumper) and `loadorder-smoke` |

`roundtrip-gate <Data folder>` parses the five vanilla masters and requires each to re-emit
byte-identically; it needs a local game install, so it is not part of CTest.

## The schema

Record layouts are described once, as YAML, in [`schema/`](schema). The descriptor language is
specified in [docs/schema-language.md](docs/schema-language.md). `tools/schemagen` compiles the
schema into:

- [`schema.json`](schema.json), the whole schema resolved into one file, for tools in any language;
- `include/ESPER/generated/`, the engine's tables and the typed views (`esper::view`).

All of these are committed, so consumers never need Python. To change the schema, you need
Python 3 and PyYAML:

```
# edit schema/..., then:
cmake --build build --target esper-schemagen      # regenerate (commit the result)
ctest --test-dir build -C RelWithDebInfo          # includes the freshness + validation checks
roundtrip-gate --schema "<Skyrim>/Data"           # the real-data gate (ESPER_BUILD_TOOLS=ON)
```

```cpp
esper::Record rec;
esper::Record::Decode(plugin, node, rec);
esper::view::Armor armor(rec);
if (auto slot = armor.MaleModel()) use(slot->Path());
armor.SetEditorId("MyArmor");                      // only that subrecord is re-encoded
esper::Record::Commit(plugin, node, rec);
```

## Status

ESPER is moving from hand-written typed record structs (`Records*.h`) to the schema-driven
engine (`esper::Record`); the plan is in [docs/schema-design.md](docs/schema-design.md). Both
paths ship side by side until the migration completes. The schema covers all 120 record types the
typed path does, and round-trips the entire vanilla load order byte-exactly. Conditions, VMAD
script data and tint layers are fully typed. Next comes modeling the sections that are still
opaque. The public API will change while that lands.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Note that this applies to anything you distribute
that links ESPER, including SKSE plugins. The vendored miniz is MIT
([third_party/miniz/LICENSE](third_party/miniz/LICENSE)).
