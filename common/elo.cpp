#include "common/elo.hpp"

#include "common/gsprt.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace capi::stats {

double logistic_elo(double score) {
    constexpr double kEpsilon = 1e-3;
    score = std::clamp(score, kEpsilon, 1 - kEpsilon);
    return -400.0 * std::log10(1.0 / score - 1.0);
}

double phi(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

double phi_inv(double p) {
    if (!(p > 0 && p < 1)) throw std::domain_error("phi_inv: p must be in (0, 1)");
    double lo = -40, hi = 40;
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (mid <= lo || mid >= hi) break;
        if (phi(mid) < p) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

EloEstimate elo_estimate(const Pentanomial& results) {
    // Porte de stat_util.get_elo: média e variância POR JOGO derivadas da
    // distribuição de pares, com as mesmas frequências regularizadas.
    const PentaPdf pdf = to_pdf(results);
    const double games = 2.0 * pdf.count;

    double mu = 0;
    for (int i = 0; i < 5; ++i) mu += pdf.probs[i] * pdf.count * (i / 2.0);
    mu /= games;

    const double mu_pair = 2.0 * mu;
    double var = 0;
    for (int i = 0; i < 5; ++i) {
        const double d = i / 2.0 - mu_pair;
        var += pdf.probs[i] * pdf.count * d * d;
    }
    var /= games;

    const double stderr_mu = std::sqrt(var) / std::sqrt(games);
    const double mu_min = mu + phi_inv(0.025) * stderr_mu;
    const double mu_max = mu + phi_inv(0.975) * stderr_mu;

    EloEstimate e{};
    e.elo = logistic_elo(mu);
    e.ci_low = logistic_elo(mu_min);
    e.ci_high = logistic_elo(mu_max);
    e.ci95_half = (e.ci_high - e.ci_low) / 2.0;
    e.los = phi((mu - 0.5) / stderr_mu);

    // Elo normalizado: t-value por jogo = (mu - 1/2) / (sqrt(2) * sigma_par).
    const auto [pmu, pvar] = moments(pdf.values, pdf.probs);
    e.nelo = (pmu - 0.5) / std::sqrt(2.0 * pvar) * kNeloDividedByNt;
    return e;
}

}  // namespace capi::stats
