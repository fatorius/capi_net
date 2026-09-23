#pragma once

#include "common/penta.hpp"

namespace capi::stats {

// Conversão entre Elo normalizado e t-value (Fishtest: nelo_divided_by_nt).
inline constexpr double kNeloDividedByNt = 347.43558552260146;  // 800 / ln(10)

// GSPRT sobre resultados pentanomiais com bounds em Elo NORMALIZADO.
// Porte direto de LLRcalc.LLR_normalized do Fishtest (estatística "t_value"):
// https://official-stockfish.github.io/docs/fishtest-wiki/Fishtest-Mathematics.html
// Sem pares, retorna 0.
double llr_normalized(double nelo0, double nelo1, const Pentanomial& results);

// Fronteiras de Wald (plano §10.3).
struct SprtBounds {
    double lower;  // log(beta / (1 - alpha))  -> aceitar H0
    double upper;  // log((1 - beta) / alpha)  -> aceitar H1
};
SprtBounds sprt_bounds(double alpha, double beta);

enum class SprtDecision { Continue, AcceptH1, AcceptH0 };
SprtDecision sprt_decide(double llr, const SprtBounds& bounds);

}  // namespace capi::stats
