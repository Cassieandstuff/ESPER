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
| `ESPER_BUILD_TOOLS` | OFF | `roundtrip-gate`, the real-data gate |

`roundtrip-gate <Data folder>` parses the five vanilla masters and requires each to re-emit
byte-identically; it needs a local game install, so it is not part of CTest.

## Status

The schema system is moving from hand-written typed record structs to a fully schema-driven
model (record and subrecord layouts described once as data, compiled into the library). The
public API will change while that lands.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Note that this applies to anything you distribute
that links ESPER, including SKSE plugins. The vendored miniz is MIT
([third_party/miniz/LICENSE](third_party/miniz/LICENSE)).
