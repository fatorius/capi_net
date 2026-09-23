#include "server/spec.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace capi::server {

using nlohmann::json;

namespace {

double parse_seconds(std::string_view s, const std::string& tc) {
    if (s.empty()) throw SpecError("invalid tc: " + tc);
    for (char c : s) {
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) {
            throw SpecError("invalid tc: " + tc);
        }
    }
    const std::string str(s);
    char* end = nullptr;
    const double v = std::strtod(str.c_str(), &end);
    if (end != str.c_str() + str.size() || !std::isfinite(v)) throw SpecError("invalid tc: " + tc);
    return v;
}

const json* get(const json& j, const char* key) {
    auto it = j.find(key);
    return (it == j.end() || it->is_null()) ? nullptr : &*it;
}

std::string req_string(const json& j, const char* key) {
    const json* v = get(j, key);
    if (!v) throw SpecError(std::string("missing field: ") + key);
    if (!v->is_string()) throw SpecError(std::string(key) + " must be a string");
    return v->get<std::string>();
}

std::optional<std::string> opt_string(const json& j, const char* key) {
    if (!get(j, key)) return std::nullopt;
    return req_string(j, key);
}

std::optional<long long> opt_integer(const json& j, const char* key) {
    const json* v = get(j, key);
    if (!v) return std::nullopt;
    if (!v->is_number_integer()) throw SpecError(std::string(key) + " must be an integer");
    return v->get<long long>();
}

std::optional<double> opt_number(const json& j, const char* key) {
    const json* v = get(j, key);
    if (!v) return std::nullopt;
    if (!v->is_number()) throw SpecError(std::string(key) + " must be a number");
    return v->get<double>();
}

}  // namespace

TimeControl parse_tc(const std::string& tc) {
    const auto plus = tc.find('+');
    const std::string_view sv(tc);
    const double base = parse_seconds(sv.substr(0, plus), tc);
    const double inc = plus == std::string::npos ? 0.0 : parse_seconds(sv.substr(plus + 1), tc);
    const long long base_ms = std::llround(base * 1000.0);
    const long long inc_ms = std::llround(inc * 1000.0);
    if (base_ms <= 0) throw SpecError("tc base must be positive: " + tc);
    if (base_ms > 24LL * 3600 * 1000 || inc_ms > 3600LL * 1000) {
        throw SpecError("tc out of range: " + tc);
    }
    return {static_cast<int>(base_ms), static_cast<int>(inc_ms)};
}

bool valid_ref(const std::string& ref) {
    if (ref.empty() || ref.size() > 255 || ref.front() == '-' || ref.front() == '/' ||
        ref.back() == '/' || ref.find("..") != std::string::npos) {
        return false;
    }
    for (char c : ref) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '/' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_book_name(const std::string& name) {
    if (name.empty() || name.size() > 255 || name.front() == '.') return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

TestSpec parse_test_spec(const json& body, const SprtPresets& presets) {
    if (!body.is_object()) throw SpecError("body must be a JSON object");
    TestSpec s;

    s.kind = req_string(body, "kind");
    if (s.kind != "sprt" && s.kind != "gauntlet") throw SpecError("kind must be sprt or gauntlet");

    s.candidate_ref = req_string(body, "candidate_ref");
    s.baseline_ref = req_string(body, "baseline_ref");
    for (const auto* ref : {&s.candidate_ref, &s.baseline_ref}) {
        if (!valid_ref(*ref)) throw SpecError("invalid git ref: " + *ref);
    }

    const auto tc = parse_tc(req_string(body, "tc"));
    s.tc_base_ms = tc.base_ms;
    s.tc_increment_ms = tc.increment_ms;

    if (auto h = opt_integer(body, "hash_mb")) {
        if (*h < 1 || *h > 4096) throw SpecError("hash_mb must be in [1, 4096]");
        s.hash_mb = static_cast<int>(*h);
    }

    s.book_name = req_string(body, "book_name");
    if (!valid_book_name(s.book_name)) throw SpecError("invalid book_name: " + s.book_name);

    const auto pairs = opt_integer(body, "total_pairs");
    if (!pairs) throw SpecError("missing field: total_pairs");
    if (*pairs < 1 || *pairs > 10'000'000) throw SpecError("total_pairs must be positive");
    s.total_pairs = static_cast<int>(*pairs);

    if (auto p = opt_integer(body, "priority")) s.priority = static_cast<int>(*p);
    s.submitted_by = opt_string(body, "submitted_by");

    const bool has_bounds = get(body, "sprt_elo0") || get(body, "sprt_elo1") ||
                            get(body, "sprt_alpha") || get(body, "sprt_beta");
    if (s.kind == "gauntlet") {
        if (get(body, "preset") || has_bounds) {
            throw SpecError("gauntlet does not take preset or SPRT bounds");
        }
    } else {
        s.sprt_preset = opt_string(body, "preset");
        if (!s.sprt_preset) throw SpecError("missing field: preset (gainer, nonreg or custom)");
        if (*s.sprt_preset == "custom") {
            s.sprt_elo0 = opt_number(body, "sprt_elo0");
            s.sprt_elo1 = opt_number(body, "sprt_elo1");
            if (!s.sprt_elo0 || !s.sprt_elo1) {
                throw SpecError("preset custom requires sprt_elo0 and sprt_elo1");
            }
            s.sprt_alpha = opt_number(body, "sprt_alpha").value_or(0.05);
            s.sprt_beta = opt_number(body, "sprt_beta").value_or(0.05);
        } else if (*s.sprt_preset == "gainer" || *s.sprt_preset == "nonreg") {
            if (has_bounds) throw SpecError("SPRT bounds are only accepted with preset custom");
            const auto& b = *s.sprt_preset == "gainer" ? presets.gainer : presets.nonreg;
            s.sprt_elo0 = b.elo0;
            s.sprt_elo1 = b.elo1;
            s.sprt_alpha = b.alpha;
            s.sprt_beta = b.beta;
        } else {
            throw SpecError("preset must be gainer, nonreg or custom");
        }
        if (!(*s.sprt_elo0 < *s.sprt_elo1)) throw SpecError("sprt_elo0 must be < sprt_elo1");
        for (double p : {*s.sprt_alpha, *s.sprt_beta}) {
            if (!(p > 0 && p < 1)) throw SpecError("sprt_alpha and sprt_beta must be in (0, 1)");
        }
    }

    s.name = opt_string(body, "name").value_or(s.candidate_ref + " vs " + s.baseline_ref);
    if (s.name.empty()) throw SpecError("name must not be empty");
    return s;
}

}  // namespace capi::server
