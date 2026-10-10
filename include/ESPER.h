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
//   Records      — esper::Record, one generic engine over the generated schema
//                  tables (schema/ → generated/Tables.h), with typed views
//                  (esper::view, made with view::As<V>). Unmodeled subrecords
//                  pass through verbatim, so every record round-trips byte-exact.
//   Write        — SerializePlugin / SavePlugin (verbatim-unless-modified) and
//                  the mutation API (CreateEmptyPlugin, CreateRecord, MakeRef,
//                  EnsureMaster, Record::Commit, FinalizePlugin).
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

// Write / mutation API
#include <ESPER/Writer.h>

// Schema-driven records: esper::Record + generated typed views (esper::view,
// made with view::As<V>).
#include <ESPER/Record.h>
#include <ESPER/generated/Views.h>
