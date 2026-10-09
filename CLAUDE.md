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

## Direction: schema-first

ESPER's product is a language-neutral schema of the plugin format. The C++ engine is its
reference implementation and lives in the same repo, because the engine's gate is what verifies
the schema. Design: [docs/schema-design.md](docs/schema-design.md), including the migration plan
and its current step. Spec: [docs/schema-language.md](docs/schema-language.md).

- **Engine:** `esper::Record` (`include/ESPER/Record.h`, `cpp/engine/`) interprets generated
  tables. It's one generic engine; never add per-record code to it.
- **Old path:** the typed `Records*.h` path still ships until the migration's final step. Don't
  extend it; new coverage goes in `schema/`.
- **No host-language escape hatches.** If the language can't express something, the language
  grows (spec + generator + engine).

## Schema workflow

- **Source of truth:** `schema/` (YAML) plus `docs/schema-language.md` (the spec). Never edit
  `include/ESPER/generated/`, `tests/generated/` or `schema.json` by hand. Regenerate them with
  the `esper-schemagen` target and commit them in the same change as the YAML. CTest's
  `*-fresh` checks fail on stale output.
- **Real-data gate:** a descriptor change must pass `roundtrip-gate --schema <Data>`. That means
  byte-exact decode and encode (verbatim and forced re-encode), no more raw subrecords than the
  typed path, FormID parity with the typed path, and every `to:` link target verified.
- **Claims about data:** `to:` and `formIds: none` describe the data, so add them only when the
  gate confirms them on the vanilla masters. Clean-room: never take layouts from xEdit or
  Mutagen definitions.
- **Language changes** (a new construct, or a changed meaning) update the spec, the generator's
  validation, the engine and `tests/schema` together, and bump `LANGUAGE_VERSION`.

## Bug log — sync is mandatory

`docs/bugs.md`: symptom, what's ruled out, root cause, exact `file:line`, status
(`OPEN`/`INVESTIGATING`/`FIXED`). Add an entry when you find a bug, flip it to `FIXED` (with the
commit) when you fix one, in the same change as the code.
