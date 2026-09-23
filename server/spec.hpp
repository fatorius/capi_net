#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>

namespace capi::server {

struct SprtPresetBounds {
    double elo0, elo1, alpha, beta;
};

// Presets de SPRT (plano §5.8), em Elo NORMALIZADO.
struct SprtPresets {
    SprtPresetBounds gainer{0.0, 5.0, 0.05, 0.05};
    SprtPresetBounds nonreg{-5.0, 0.0, 0.05, 0.05};
};

// Spec validada de um teste, ainda com refs (não SHAs).
struct TestSpec {
    std::string name;
    std::string kind;  // 'sprt' | 'gauntlet'
    std::string candidate_ref, baseline_ref;
    int tc_base_ms = 0, tc_increment_ms = 0;
    int hash_mb = 16;
    std::string book_name;
    int total_pairs = 0;
    std::optional<std::string> sprt_preset;  // 'gainer' | 'nonreg' | 'custom'
    std::optional<double> sprt_elo0, sprt_elo1, sprt_alpha, sprt_beta;
    int priority = 0;
    std::optional<std::string> submitted_by;
};

struct SpecError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// "10+0.1" (segundos) -> {10000, 100} ms; "10" = "10+0". Base > 0, inc >= 0.
struct TimeControl {
    int base_ms, increment_ms;
};
TimeControl parse_tc(const std::string& tc);

// Git ref aceito na API: [A-Za-z0-9._/-], sem começar com '-' e sem "..".
bool valid_ref(const std::string& ref);

// Nome de arquivo simples dentro do diretório de books (sem caminhos).
bool valid_book_name(const std::string& name);

// Valida o corpo de POST /api/admin/tests (§5.1, passo 3) — tudo que não
// depende de rede nem do conteúdo do book. Lança SpecError.
TestSpec parse_test_spec(const nlohmann::json& body, const SprtPresets& presets);

}  // namespace capi::server
