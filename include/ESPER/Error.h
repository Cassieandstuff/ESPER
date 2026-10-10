#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Errors. Every fallible ESPER call returns Result<T>: the value, or an Error
// saying why not.
//
//   auto rec = esper::Record::Decode(plugin, node);
//   if (!rec) return log(rec.error().message);
//   esper::view::Armor armor = *esper::view::As<esper::view::Armor>(*rec);
// ─────────────────────────────────────────────────────────────────────────────
#include <expected>
#include <format>
#include <string>
#include <utility>

namespace esper {

struct Error {
    std::string message;
};

template <class T = void>
using Result = std::expected<T, Error>;

// `return Fail("cannot open {}", path);`
template <class... Args>
std::unexpected<Error> Fail(std::format_string<Args...> fmt, Args&&... args) {
    return std::unexpected(Error{ std::format(fmt, std::forward<Args>(args)...) });
}

// Prefixes an error with context: `return Wrap(r.error(), "loading {}", name);`
template <class... Args>
std::unexpected<Error> Wrap(const Error& inner, std::format_string<Args...> fmt, Args&&... args) {
    return std::unexpected(Error{ std::format(fmt, std::forward<Args>(args)...) + ": " + inner.message });
}

} // namespace esper
