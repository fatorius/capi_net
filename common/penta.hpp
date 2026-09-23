#pragma once

#include <array>
#include <cstdint>

namespace capi::stats {

// Categorias pentanomiais, do ponto de vista do candidato (plano §4.1, §10.1).
enum class Penta : int { LL = 0, LD = 1, DD_WL = 2, WD = 3, WW = 4 };

// Mapeia a pontuação do par (0, 0.5, 1.0, 1.5, 2.0) para a categoria.
// Lança std::invalid_argument para qualquer outro valor.
Penta category_from_pair_score(double pair_score);

struct Pentanomial {
    // Frequências indexadas por Penta: [LL, LD, DD/WL, WD, WW].
    std::array<std::int64_t, 5> counts{};

    void add(Penta p, std::int64_t n = 1) { counts[static_cast<int>(p)] += n; }
    void add_pair_score(double pair_score) { add(category_from_pair_score(pair_score)); }

    std::int64_t operator[](Penta p) const { return counts[static_cast<int>(p)]; }
    std::int64_t pairs() const;

    Pentanomial& operator+=(const Pentanomial& other);
};

// Distribuição discreta sobre os 5 valores de score por jogo {0, .25, .5, .75, 1}.
// Frequências zero são substituídas por 1e-3 (regularização idêntica ao
// LLRcalc.regularize do Fishtest), de modo que nenhuma probabilidade é zero.
struct PentaPdf {
    double count = 0;                 // soma das frequências regularizadas
    std::array<double, 5> values{};   // a_i = i / 4
    std::array<double, 5> probs{};    // p_i
};

PentaPdf to_pdf(const Pentanomial& p);

// Média e variância de uma distribuição discreta.
struct Moments {
    double mean;
    double var;
};
Moments moments(const std::array<double, 5>& values, const std::array<double, 5>& probs);

}  // namespace capi::stats
