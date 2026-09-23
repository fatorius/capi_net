#pragma once

#include "common/penta.hpp"

namespace capi::stats {

struct EloEstimate {
    double elo;         // Elo logístico (stat_util.get_elo do Fishtest)
    double ci95_half;   // meia-largura simétrica do IC 95%, como o Fishtest exibe
    double ci_low;      // limites do IC 95% em Elo logístico (assimétricos)
    double ci_high;
    double los;         // likelihood of superiority
    double nelo;        // Elo normalizado (mesma escala dos bounds do SPRT)
};

// Elo e IC a partir da distribuição pentanomial (variância pentanomial).
EloEstimate elo_estimate(const Pentanomial& results);

// Score esperado -> Elo logístico, com clamp em [1e-3, 1 - 1e-3].
double logistic_elo(double score);

// CDF normal padrão e sua inversa.
double phi(double x);
double phi_inv(double p);

}  // namespace capi::stats
