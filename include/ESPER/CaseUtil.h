#pragma once
#include <cctype>
#include <cstdint>
#include <string>

namespace esper {

// Case-insensitive string utilities (ASCII). Bethesda filenames, EditorIDs and
// plugin names compare case-insensitively everywhere in the ecosystem; this is
// the ONE home for those helpers (previously re-cut per file across the tree).

inline char LowerCh(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline std::string ToLowerCI(std::string s) {
    for (char& c : s) c = LowerCh(c);
    return s;
}

inline bool EqualCI(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (LowerCh(a[i]) != LowerCh(b[i])) return false;
    return true;
}

// Three-way compare: <0, 0, >0. Shorter string sorts first on shared prefix.
inline int CompareCI(const std::string& a, const std::string& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const int ca = LowerCh(a[i]);
        const int cb = LowerCh(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

inline bool StartsWithCI(const std::string& s, const char* prefix) {
    size_t n = 0;
    while (prefix[n]) ++n;
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (LowerCh(s[i]) != LowerCh(prefix[i])) return false;
    return true;
}

inline bool ContainsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j)
            if (LowerCh(haystack[i + j]) != LowerCh(needle[j])) break;
        if (j == needle.size()) return true;
    }
    return false;
}

// FNV-1a 64-bit over the lowercased string.
inline uint64_t HashCI(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (char c : s) {
        h ^= static_cast<uint8_t>(LowerCh(c));
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace esper
