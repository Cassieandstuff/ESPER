#pragma once
#include <ESPER/Error.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace esper {

// A stable, plugin-relative reference to a record: the owning plugin's filename
// plus the record's local (master-relative) FormID.
struct FormKey {
    std::string plugin;        // owning plugin filename, e.g. "Skyrim.esm"
    uint32_t    localId = 0;   // master-relative FormID (low 24 bits significant)

    bool valid() const { return !plugin.empty(); }

    // Canonical form: 6-hex UPPERCASE of (localId & 0x00FFFFFF), colon, plugin
    // filename. e.g. {"Skyrim.esm", 0x013BBA} -> "013BBA:Skyrim.esm".
    std::string ToString() const;

    // The inverse of ToString. Accepts any number of hex digits (with or
    // without 0x) up to 24 bits: "13BBA:Skyrim.esm", "0x013BBA:Skyrim.esm".
    static Result<FormKey> Parse(std::string_view text);
};

} // namespace esper
