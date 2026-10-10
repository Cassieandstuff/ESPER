#pragma once
#include <ESPER/SchemaTypes.h>
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

    // Looks `id` up in the plugin's sidecar for `table` (None reads as Strings).
    bool Find(schema::StrTable table, uint32_t id, std::string& out) {
        const int v = Slot(table);
        if (!loaded_[v]) Load(v);
        return tables_[v].Find(id, out);
    }

private:
    // Slots: 0 .STRINGS, 1 .ILSTRINGS, 2 .DLSTRINGS.
    static int Slot(schema::StrTable t) {
        switch (t) {
        case schema::StrTable::ILStrings: return 1;
        case schema::StrTable::DLStrings: return 2;
        default:                          return 0;
        }
    }
    static const char* Extension(int v) {
        static const char* kExt[3] = { ".STRINGS", ".ILSTRINGS", ".DLSTRINGS" };
        return kExt[v];
    }

    void Load(int v) {
        loaded_[v] = true;
        if (!fetch_) return;

        std::string base = plugin_;
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base.resize(dot);

        const std::string path =
            "Strings/" + base + "_" + language_ + Extension(v);
        std::vector<uint8_t> bytes;
        if (!fetch_(path, bytes)) return;
        // .STRINGS is NUL-terminated framing; IL/DL are length-prefixed.
        tables_[v].Parse(bytes, v != 0);
    }

    StringsTable tables_[3];
    bool         loaded_[3] = { false, false, false };
    std::string  plugin_;
    std::string  language_ = "English";
    FetchFn      fetch_;
};

} // namespace esper
