#include <ESPER/FormKey.h>

#include <charconv>
#include <cstdio>

namespace esper {

std::string FormKey::ToString() const {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06X", localId & 0x00FFFFFFu);
    return std::string(buf) + ":" + plugin;
}

Result<FormKey> FormKey::Parse(std::string_view text) {
    const size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size())
        return Fail("malformed FormKey '{}' (expected HEXID:Plugin.esp)", text);
    std::string_view hex = text.substr(0, colon);
    if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex.remove_prefix(2);
    uint32_t id = 0;
    const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
    if (ec != std::errc{} || end != hex.data() + hex.size())
        return Fail("malformed FormKey '{}': '{}' isn't a hex id", text, text.substr(0, colon));
    if (id > 0x00FFFFFFu)
        return Fail("malformed FormKey '{}': the id has a load-order byte; FormKeys hold the "
                    "24-bit local id", text);
    return FormKey{ std::string(text.substr(colon + 1)), id };
}

} // namespace esper
