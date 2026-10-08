#include <ESPER/Zlib.h>

#include <cstdarg>
#include <cstdio>
#include <limits>

#include "miniz.h"

namespace esper {

static void SetErr(char* err, int errLen, const char* fmt, ...) {
    if (!err || errLen <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, static_cast<size_t>(errLen), fmt, ap);
    va_end(ap);
}

bool ZlibInflate(const uint8_t* src, size_t srcLen, size_t expectedOut,
                 std::vector<uint8_t>& out, char* err, int errLen) {
    out.clear();

    if (!src || srcLen == 0) {
        SetErr(err, errLen, "ZlibInflate: empty/null source");
        return false;
    }

    if (expectedOut == 0) {
        return true;
    }

    if (expectedOut > 256u * 1024u * 1024u) {
        SetErr(err, errLen, "ZlibInflate: decompressed size %zu exceeds cap",
               static_cast<size_t>(expectedOut));
        return false;
    }

    const mz_ulong kULongMax = std::numeric_limits<mz_ulong>::max();
    if (srcLen > static_cast<size_t>(kULongMax) ||
        expectedOut > static_cast<size_t>(kULongMax)) {
        SetErr(err, errLen, "ZlibInflate: size exceeds mz_ulong range");
        return false;
    }

    out.resize(expectedOut);

    mz_ulong destLen = static_cast<mz_ulong>(expectedOut);
    const int rc = mz_uncompress(out.data(), &destLen,
                                 src, static_cast<mz_ulong>(srcLen));
    if (rc != MZ_OK) {
        SetErr(err, errLen, "ZlibInflate: mz_uncompress failed (%d: %s)",
               rc, mz_error(rc) ? mz_error(rc) : "unknown");
        out.clear();
        return false;
    }

    if (static_cast<size_t>(destLen) != expectedOut) {
        SetErr(err, errLen,
               "ZlibInflate: size mismatch (got %llu, expected %llu)",
               static_cast<unsigned long long>(destLen),
               static_cast<unsigned long long>(expectedOut));
        out.clear();
        return false;
    }

    return true;
}

bool ZlibDeflate(const uint8_t* src, size_t srcLen,
                 std::vector<uint8_t>& out, char* err, int errLen) {
    out.clear();
    if (srcLen == 0) return true;
    if (!src) {
        SetErr(err, errLen, "ZlibDeflate: null source");
        return false;
    }

    const mz_ulong kULongMax = std::numeric_limits<mz_ulong>::max();
    if (srcLen > static_cast<size_t>(kULongMax)) {
        SetErr(err, errLen, "ZlibDeflate: size exceeds mz_ulong range");
        return false;
    }

    const mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(srcLen));
    out.resize(static_cast<size_t>(bound));
    mz_ulong destLen = bound;
    const int rc = mz_compress(out.data(), &destLen,
                               src, static_cast<mz_ulong>(srcLen));
    if (rc != MZ_OK) {
        SetErr(err, errLen, "ZlibDeflate: mz_compress failed (%d: %s)",
               rc, mz_error(rc) ? mz_error(rc) : "unknown");
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(destLen));
    return true;
}

} // namespace esper
