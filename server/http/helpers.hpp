#pragma once

#include "server/log.hpp"
#include "server/types.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <climits>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

// Helpers compartilhados pelos handlers HTTP.
namespace capi::server::http {

using nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers de requisição/resposta
// ---------------------------------------------------------------------------
struct BadRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline const std::set<std::string> kOutcomes{"candidate_win", "draw", "candidate_loss"};
inline const std::set<std::string> kTerminations{"normal", "timeout", "illegal_move",
                                          "crash",  "adjudication", "other"};
inline const std::set<std::string> kPinningModes{"pinned", "homogeneous_unpinned", "qos_hint"};

inline void send_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

inline void send_error(httplib::Response& res, int status, const std::string& error,
                const std::string& reason = {}) {
    json body{{"error", error}};
    if (!reason.empty()) body["reason"] = reason;
    send_json(res, status, body);
}

inline json parse_body(const httplib::Request& req) {
    json j = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) throw BadRequest("body must be a JSON object");
    return j;
}

inline const json& field(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) throw BadRequest(std::string("missing field: ") + key);
    return *it;
}

inline std::int64_t req_int64(const json& j, const char* key) {
    const auto& v = field(j, key);
    if (!v.is_number_integer()) throw BadRequest(std::string(key) + " must be an integer");
    return v.get<std::int64_t>();
}

inline int req_int(const json& j, const char* key, int min_value) {
    const auto v = req_int64(j, key);
    if (v < min_value || v > INT32_MAX) throw BadRequest(std::string(key) + " out of range");
    return static_cast<int>(v);
}

inline std::optional<int> opt_int(const json& j, const char* key, int min_value) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    return req_int(j, key, min_value);
}

inline double req_double(const json& j, const char* key) {
    const auto& v = field(j, key);
    if (!v.is_number()) throw BadRequest(std::string(key) + " must be a number");
    return v.get<double>();
}

inline std::optional<double> opt_double(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    return req_double(j, key);
}

inline bool req_bool(const json& j, const char* key) {
    const auto& v = field(j, key);
    if (!v.is_boolean()) throw BadRequest(std::string(key) + " must be a boolean");
    return v.get<bool>();
}

inline std::string req_string(const json& j, const char* key) {
    const auto& v = field(j, key);
    if (!v.is_string()) throw BadRequest(std::string(key) + " must be a string");
    return v.get<std::string>();
}

inline std::optional<std::string> opt_string(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    return req_string(j, key);
}

inline std::string req_enum(const json& j, const char* key, const std::set<std::string>& allowed) {
    auto v = req_string(j, key);
    if (!allowed.contains(v)) throw BadRequest(std::string("invalid ") + key + ": " + v);
    return v;
}

inline std::int64_t path_id(const httplib::Request& req) {
    try {
        return std::stoll(req.matches[1].str());
    } catch (const std::exception&) {
        throw BadRequest("invalid id in path");
    }
}

template <typename T>
inline json nullable(const std::optional<T>& v) {
    return v ? json(*v) : json(nullptr);
}

using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

// Converte exceções em respostas JSON: 400 para entrada inválida, 503 para
// banco indisponível, 500 para o resto (com log).
inline Handler guarded(Handler h) {
    return [h = std::move(h)](const httplib::Request& req, httplib::Response& res) {
        try {
            h(req, res);
        } catch (const BadRequest& e) {
            send_error(res, 400, "bad_request", e.what());
        } catch (const pqxx::broken_connection& e) {
            log_error(req.method + " " + req.path + ": database unavailable: " + e.what());
            send_error(res, 503, "database_unavailable");
        } catch (const std::exception& e) {
            log_error(req.method + " " + req.path + ": " + e.what());
            send_error(res, 500, "internal_error");
        }
    };
}

inline json adjudication_json(const Adjudication& a) {
    json out{{"draw", nullptr}, {"resign", nullptr}};
    if (a.draw) {
        out["draw"] = {{"movenumber", a.draw->movenumber},
                       {"movecount", a.draw->movecount},
                       {"score_cp", a.draw->score_cp}};
    }
    if (a.resign) {
        out["resign"] = {{"movecount", a.resign->movecount},
                         {"score_cp", a.resign->score_cp},
                         {"twosided", a.resign->twosided}};
    }
    return out;
}

}  // namespace capi::server::http
