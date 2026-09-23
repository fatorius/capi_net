#pragma once

#include <pqxx/pqxx>

#include <optional>

namespace capi::server::db {

template <typename T>
std::optional<T> opt(const pqxx::field& f) {
    if (f.is_null()) return std::nullopt;
    return f.as<T>();
}

}  // namespace capi::server::db
