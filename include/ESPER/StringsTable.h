#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace esper {

// Parses a Bethesda localized-strings sidecar (.STRINGS / .ILSTRINGS /
// .DLSTRINGS) byte buffer into an id->string table.
class StringsTable {
public:
    StringsTable() = default;

    // Parse `data[0..size)`. `lengthPrefixed` selects .IL/.DL framing (true)
    // vs plain .STRINGS null-terminated framing (false).
    bool Parse(const uint8_t* data, size_t size, bool lengthPrefixed);

    bool Parse(const std::vector<uint8_t>& bytes, bool lengthPrefixed) {
        return Parse(bytes.data(), bytes.size(), lengthPrefixed);
    }

    bool Find(uint32_t id, std::string& out) const {
        auto it = map_.find(id);
        if (it == map_.end()) return false;
        out = it->second;
        return true;
    }

    size_t Size() const { return map_.size(); }

private:
    std::unordered_map<uint32_t, std::string> map_;
};

} // namespace esper
