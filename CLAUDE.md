# ESPER — working notes

Clean-room C++23 Bethesda plugin (ESM/ESP/ESL) library for Skyrim SE/AE. Extracted from the
Skyrim Content Tools monorepo (`ESPER/`, previously the `bethesda-plugins` target / `bethesda`
namespace) with fresh history. GPL-3.0-or-later.

## Public surface

- Consumers link `esper::esper` and `#include <ESPER.h>` — nothing else. Component headers under
  `include/ESPER/` exist to organize the umbrella, not as separate entry points.
- Everything is in `namespace esper`.
- The library never sets `CMAKE_MSVC_RUNTIME_LIBRARY`; it must build under the consumer's choice
  (static-CRT SKSE plugins and dynamic-CRT desktop tools both consume it).
- Stay a dependency-free leaf. miniz is vendored and exported as `esper::miniz`.

## Fidelity doctrine (load-bearing)

- **Round-trip or gate.** Decode → encode must reproduce the original payload byte-for-byte for
  every record in the vanilla load order. Anything the schema cannot reproduce exactly is
  *refused* and kept as verbatim passthrough, so fidelity never depends on schema completeness.
- One description drives every direction (decode, encode, FormID visitation, presence). Never add
  a second, hand-written path for one direction.
- FormID remap refuses rather than guesses: unknown passthrough bytes or an unclassifiable CTDA
  parameter make a record remap-unsafe.
- `esper-test` (synthetic fixtures, CTest) proves the code; `roundtrip-gate <Data>`
  (`-DESPER_BUILD_TOOLS=ON`) proves it on real masters. Schema changes must pass both.

## Direction: schema-driven (option B)

The current `Records*.h` family is hand-written structs bound to schema tables by member pointer,
with subrecord internals as `#pragma pack` structs and FormID offsets listed by hand. The target:
record AND subrecord layouts described once as data (in-repo descriptor files), compiled into
constexpr tables at build time, interpreted by one generic decode/encode/visit engine over a
dynamic record model. No runtime schema files and no new consumer-facing dependencies. Unions
(layouts chosen by size, form version, or a sibling value) need first-class expression in the
descriptor language. Typed accessors, if any, are generated from the same descriptors.

## Bug log — sync is mandatory

`docs/bugs.md`: symptom, what's ruled out, root cause, exact `file:line`, status
(`OPEN`/`INVESTIGATING`/`FIXED`). Add an entry when you find a bug, flip it to `FIXED` (with the
commit) when you fix one, in the same change as the code.
