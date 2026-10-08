#pragma once
#include <ESPER/Schema.h>
#include <ESPER/StringsTable.h>

#include <functional>
#include <string>
#include <vector>

namespace esper {

// Language-parameterized localized-strings lookup for ONE plugin: the
// {.STRINGS, .ILSTRINGS, .DLSTRINGS} sidecar triple. Files are fetched lazily
// per variant through the caller's resolver (loose file or BSA — the library
// stays archive-agnostic) and parsed once. A missing sidecar simply yields
// misses; it is retried only if the set is re-Init'd.
class StringsSet {
public:
    // Resolves a data-relative path (e.g. "Strings/Skyrim_English.STRINGS")
    // to raw bytes.
    using FetchFn = std::function<bool(const std::string& relPath,
                                       std::vector<uint8_t>& outBytes)>;

    void Init(std::string pluginFilename, std::string language, FetchFn fetch) {
        plugin_   = std::move(pluginFilename);
        language_ = std::move(language);
        fetch_    = std::move(fetch);
        for (int i = 0; i < 3; ++i) {
            tables_[i] = StringsTable{};
            loaded_[i] = false;
        }
    }

    bool Find(StrVariant variant, uint32_t id, std::string& out) {
        const int v = static_cast<int>(variant);
        if (!loaded_[v]) Load(variant);
        return tables_[v].Find(id, out);
    }

    // Resolves an Lstring decoded from a LOCALIZED plugin (its `variant` was
    // stamped by the field's codec). Inline strings return directly.
    bool Resolve(const Lstring& s, std::string& out) {
        if (s.text) { out = *s.text; return true; }
        return Find(s.variant, s.id, out);
    }

private:
    static const char* Extension(StrVariant v) {
        switch (v) {
        case StrVariant::ILStrings: return ".ILSTRINGS";
        case StrVariant::DLStrings: return ".DLSTRINGS";
        default:                    return ".STRINGS";
        }
    }

    void Load(StrVariant variant) {
        const int v = static_cast<int>(variant);
        loaded_[v] = true;
        if (!fetch_) return;

        std::string base = plugin_;
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base.resize(dot);

        const std::string path =
            "Strings/" + base + "_" + language_ + Extension(variant);
        std::vector<uint8_t> bytes;
        if (!fetch_(path, bytes)) return;
        // .STRINGS is NUL-terminated framing; IL/DL are length-prefixed.
        tables_[v].Parse(bytes, variant != StrVariant::Strings);
    }

    StringsTable tables_[3];
    bool         loaded_[3] = { false, false, false };
    std::string  plugin_;
    std::string  language_ = "English";
    FetchFn      fetch_;
};

} // namespace esper
