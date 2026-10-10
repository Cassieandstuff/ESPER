#pragma once
#include <ESPER/Error.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace esper {

// Both directions of the zlib transform live HERE (round-trip SOP:
// co-located, byte-gated). Bethesda compressed records store
// [uint32 decompressedSize][zlib stream (RFC 1950)].

// Inflates a raw zlib stream into exactly `expectedOut` bytes.
// Caps at 256 MB to prevent allocation bombs from corrupt records.
Result<std::vector<uint8_t>> ZlibInflate(std::span<const uint8_t> src, size_t expectedOut);

// Deflates `src` into a zlib stream (default compression level). NOTE: the
// bytes will differ from Bethesda's own zlib output — the gate's compressed-
// record pass criterion is decompressed-payload equality for re-compressed
// records, byte-identity only for carried-through originals.
Result<std::vector<uint8_t>> ZlibDeflate(std::span<const uint8_t> src);

} // namespace esper
