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
// ...
```

Everything lives in `namespace esper`.

ESPER does not set the MSVC runtime library; it inherits `CMAKE_MSVC_RUNTIME_LIBRARY` from
your project, so it links cleanly into both static-CRT SKSE plugins and dynamic-CRT desktop
tools. CMake 3.28+ and a C++23 compiler are required.

`esper::miniz` is also exported, so a consumer that needs zlib itself can link that copy
instead of compiling a second one.

## Building standalone

```
cmake -S . -B build
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo
```

| Option | Default | |
|---|---|---|
| `ESPER_BUILD_TESTS` | ON when top-level | `esper-test`, the synthetic-fixture test suite (registered with CTest) |
| `ESPER_BUILD_TOOLS` | OFF | `roundtrip-gate` (the real-data gate) and `esm-dump` (a read-only record dumper) |

`roundtrip-gate <Data folder>` parses the five vanilla masters and requires each to re-emit
byte-identically; it needs a local game install, so it is not part of CTest.

## The schema

Record layouts are described once, as YAML, in [`schema/`](schema). The descriptor language is
specified in [docs/schema-language.md](docs/schema-language.md). `tools/schemagen` compiles the
schema into:

- [`schema.json`](schema.json), the whole schema resolved into one file, for tools in any language;
- `include/ESPER/generated/`, the engine's tables plus typed handles (`esper::fields`) and typed
  views (`esper::view`).

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
paths ship side by side until the migration completes. The schema currently covers KYWD, GLOB,
TXST, MISC and ARMO. The public API will change while the rest lands.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Note that this applies to anything you distribute
that links ESPER, including SKSE plugins. The vendored miniz is MIT
([third_party/miniz/LICENSE](third_party/miniz/LICENSE)).
