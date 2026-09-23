#include "common/penta.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace capi::stats {

Penta category_from_pair_score(double pair_score) {
    const double idx = pair_score * 2.0;
    const double rounded = std::round(idx);
    if (!(rounded >= 0.0 && rounded <= 4.0) || std::abs(idx - rounded) > 1e-9) {
        throw std::invalid_argument("invalid pair score: " + std::to_string(pair_score));
    }
    return static_cast<Penta>(static_cast<int>(rounded));
}

std::int64_t Pentanomial::pairs() const {
    std::int64_t n = 0;
    for (auto c : counts) n += c;
    return n;
}

Pentanomial& Pentanomial::operator+=(const Pentanomial& other) {
    for (int i = 0; i < 5; ++i) counts[i] += other.counts[i];
    return *this;
}

PentaPdf to_pdf(const Pentanomial& p) {
    constexpr double kEpsilon = 1e-3;
    std::array<double, 5> reg{};
    PentaPdf pdf;
    for (int i = 0; i < 5; ++i) {
        reg[i] = p.counts[i] == 0 ? kEpsilon : static_cast<double>(p.counts[i]);
        pdf.count += reg[i];
    }
    for (int i = 0; i < 5; ++i) {
        pdf.values[i] = i / 4.0;
        pdf.probs[i] = reg[i] / pdf.count;
    }
    return pdf;
}

Moments moments(const std::array<double, 5>& values, const std::array<double, 5>& probs) {
    double mean = 0;
    for (int i = 0; i < 5; ++i) mean += probs[i] * values[i];
    double var = 0;
    for (int i = 0; i < 5; ++i) var += probs[i] * (values[i] - mean) * (values[i] - mean);
    return {mean, var};
}

}  // namespace capi::stats
