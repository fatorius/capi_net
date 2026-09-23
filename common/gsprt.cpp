#include "common/gsprt.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace capi::stats {

namespace {

using Arr = std::array<double, 5>;

// Resolve sum_i p_i * a_i / (1 + x * a_i) = 0 (LLRcalc.secular).
// f é estritamente decrescente em (-1/max(a), -1/min(a)), então bisseção é
// robusta; iteramos até o intervalo parar de encolher.
double secular(const Arr& a, const Arr& p) {
    const double v = *std::min_element(a.begin(), a.end());
    const double w = *std::max_element(a.begin(), a.end());
    if (v * w >= 0) {
        throw std::domain_error("secular equation requires support straddling zero");
    }
    constexpr double kEpsilon = 1e-9;
    double lo = -1.0 / w + kEpsilon;
    double hi = -1.0 / v - kEpsilon;

    auto f = [&](double x) {
        double s = 0;
        for (int i = 0; i < 5; ++i) s += p[i] * a[i] / (1 + x * a[i]);
        return s;
    };

    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (mid <= lo || mid >= hi) break;
        if (f(mid) > 0) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// Estimativa de máxima verossimilhança de uma distribuição com t-value
// (mu - ref) / sigma = s, dada a distribuição empírica (LLRcalc.MLE_t_value).
Arr mle_t_value(const PentaPdf& hat, double ref, double s) {
    Arr pdf_mle;
    pdf_mle.fill(1.0 / 5.0);  // uniform
    for (int iter = 0; iter < 10; ++iter) {
        const Arr prev = pdf_mle;
        const auto [mu, var] = moments(hat.values, pdf_mle);
        const double sigma = std::sqrt(var);
        Arr a1;
        for (int i = 0; i < 5; ++i) {
            const double z = (mu - hat.values[i]) / sigma;
            a1[i] = hat.values[i] - ref - s * sigma * (1 + z * z) / 2;
        }
        const double x = secular(a1, hat.probs);
        double max_delta = 0;
        for (int i = 0; i < 5; ++i) {
            pdf_mle[i] = hat.probs[i] / (1 + x * a1[i]);
            max_delta = std::max(max_delta, std::abs(prev[i] - pdf_mle[i]));
        }
        if (max_delta < 1e-9) break;
    }
    return pdf_mle;
}

}  // namespace

double llr_normalized(double nelo0, double nelo1, const Pentanomial& results) {
    if (results.pairs() == 0) return 0.0;

    // Para pentanomial o t-value por par é sqrt(2) vezes o t-value por jogo.
    const double t0 = nelo0 / kNeloDividedByNt * std::sqrt(2.0);
    const double t1 = nelo1 / kNeloDividedByNt * std::sqrt(2.0);

    const PentaPdf pdf = to_pdf(results);
    const Arr p0 = mle_t_value(pdf, 0.5, t0);
    const Arr p1 = mle_t_value(pdf, 0.5, t1);

    double llr_per_pair = 0;
    for (int i = 0; i < 5; ++i) {
        llr_per_pair += pdf.probs[i] * (std::log(p1[i]) - std::log(p0[i]));
    }
    return pdf.count * llr_per_pair;
}

SprtBounds sprt_bounds(double alpha, double beta) {
    return {std::log(beta / (1 - alpha)), std::log((1 - beta) / alpha)};
}

SprtDecision sprt_decide(double llr, const SprtBounds& bounds) {
    if (llr >= bounds.upper) return SprtDecision::AcceptH1;
    if (llr <= bounds.lower) return SprtDecision::AcceptH0;
    return SprtDecision::Continue;
}

}  // namespace capi::stats
