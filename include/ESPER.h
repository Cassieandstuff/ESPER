#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ESPER — the ONE public header. Consumers `#include <ESPER.h>` (typically
// from their PCH, alongside <RE/Skyrim.h>) and nothing else; the component
// headers under ESPER/ are an implementation detail of this umbrella.
//
// Clean-room C++23 reader/writer for the Bethesda plugin format (ESM/ESP/ESL),
// targeting Skyrim SE/AE (VR is format-identical). Everything here is in
// namespace `esper`.
//
//   Container    — Plugin (whole-file parse → group tree + full record index),
//                  LoadOrderReader / LoadOrderIndex (override chains), FormKey.
//   Payloads     — RecordNode::Payload (lazy decompression), SubIterator
//                  (XXXX-aware subrecord walk), Zlib.
//   Schema       — one declarative table per record type; decode/encode/FormID
//                  visitation derive from it (Schema.h + the Records*.h family +
//                  Conditions/Vmad codecs). Unmodeled subrecords pass through
//                  verbatim, so every record round-trips byte-exact.
//   Write        — SerializePlugin / SavePlugin (verbatim-unless-modified) and
//                  the mutation API (CreateEmptyPlugin, CreateRecord, MakeRef,
//                  EnsureMaster, DecodeNode/CommitNode, FinalizePlugin).
//   World        — WorldIndex (WRLD→(x,y)→CELL, interiors, terrain enumeration).
//   Localization — StringsSet (language-parameterized STRINGS/IL/DL triple).
// ─────────────────────────────────────────────────────────────────────────────

// Errors (Result<T> = std::expected<T, Error>)
#include <ESPER/Error.h>

// Core container + identity
#include <ESPER/FormKey.h>
#include <ESPER/PluginFlags.h>
#include <ESPER/RecordHeader.h>
#include <ESPER/GroupTree.h>
#include <ESPER/Plugin.h>
#include <ESPER/LoadOrderReader.h>
#include <ESPER/LoadOrderIndex.h>
#include <ESPER/WorldIndex.h>

// Byte-level helpers (used by consumers that hand-walk payloads)
#include <ESPER/ByteReader.h>
#include <ESPER/ByteWriter.h>
#include <ESPER/Subrecords.h>
#include <ESPER/CaseUtil.h>
#include <ESPER/Zlib.h>

// Localized strings
#include <ESPER/StringsTable.h>
#include <ESPER/StringsSet.h>

// Schema system + codecs
#include <ESPER/Schema.h>
#include <ESPER/SchemaRegistry.h>
#include <ESPER/Conditions.h>
#include <ESPER/Vmad.h>

// Typed record schemas (waves 1-7 — the full roster)
#include <ESPER/RecordsCommon.h>
#include <ESPER/RecordsActor.h>
#include <ESPER/RecordsItem.h>
#include <ESPER/RecordsMisc.h>
#include <ESPER/RecordsWorld.h>
#include <ESPER/RecordsMagic.h>
#include <ESPER/RecordsStory.h>
#include <ESPER/RecordsFx.h>
#include <ESPER/RecordsSystem.h>
#include <ESPER/RecordsMore.h>

// Write / mutation API
#include <ESPER/Writer.h>

// Schema-driven records (the successor to the typed Records*.h family; both
// coexist until the migration in docs/schema-design.md completes):
// esper::Record + generated handles (esper::fields) + typed views (esper::view).
#include <ESPER/Record.h>
#include <ESPER/generated/Fields.h>
#include <ESPER/generated/Views.h>
