#include <ESPER/Zlib.h>

#include <limits>

#include "miniz.h"

namespace esper {

Result<std::vector<uint8_t>> ZlibInflate(std::span<const uint8_t> src, size_t expectedOut) {
    if (src.empty())
        return Fail("ZlibInflate: empty source");
    if (expectedOut == 0)
        return std::vector<uint8_t>{};
    if (expectedOut > 256u * 1024u * 1024u)
        return Fail("ZlibInflate: decompressed size {} exceeds cap", expectedOut);

    const mz_ulong kULongMax = std::numeric_limits<mz_ulong>::max();
    if (src.size() > static_cast<size_t>(kULongMax) || expectedOut > static_cast<size_t>(kULongMax))
        return Fail("ZlibInflate: size exceeds mz_ulong range");

    std::vector<uint8_t> out(expectedOut);
    mz_ulong destLen = static_cast<mz_ulong>(expectedOut);
    const int rc = mz_uncompress(out.data(), &destLen, src.data(), static_cast<mz_ulong>(src.size()));
    if (rc != MZ_OK)
        return Fail("ZlibInflate: mz_uncompress failed ({}: {})", rc, mz_error(rc) ? mz_error(rc) : "unknown");
    if (static_cast<size_t>(destLen) != expectedOut)
        return Fail("ZlibInflate: size mismatch (got {}, expected {})",
                    static_cast<unsigned long long>(destLen), expectedOut);
    return out;
}

Result<std::vector<uint8_t>> ZlibDeflate(std::span<const uint8_t> src) {
    if (src.empty()) return std::vector<uint8_t>{};

    const mz_ulong kULongMax = std::numeric_limits<mz_ulong>::max();
    if (src.size() > static_cast<size_t>(kULongMax))
        return Fail("ZlibDeflate: size exceeds mz_ulong range");

    const mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(src.size()));
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    mz_ulong destLen = bound;
    const int rc = mz_compress(out.data(), &destLen, src.data(), static_cast<mz_ulong>(src.size()));
    if (rc != MZ_OK)
        return Fail("ZlibDeflate: mz_compress failed ({}: {})", rc, mz_error(rc) ? mz_error(rc) : "unknown");
    out.resize(static_cast<size_t>(destLen));
    return out;
}

} // namespace esper
