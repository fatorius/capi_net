#include "common/elo.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace capi::stats;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
struct EloCase {
    Pentanomial results;
    double elo, elo95, los, nelo;
};
}  // namespace

// Referência: stat_util.get_elo (elo, elo95, los) do Fishtest; nelo pela
// definição de LLRcalc.LLR_normalized_alt ((mu - 1/2) / sqrt(2 var) * 800/ln10).
TEST_CASE("elo_estimate matches Fishtest reference") {
    const auto c = GENERATE(
        EloCase{{{10789, 19328, 33806, 19402, 10543}},
                -0.7735773359165423, 1.2775798957280218, 0.1176588580327751, -0.9516290044726476},
        EloCase{{{39, 2226, 31451, 2412, 40}},
                0.902981048809024, 0.6625314859816553, 0.9962220973354207, 3.450799606946096},
        EloCase{{{1187, 7410, 13475, 7378, 1164}},
                -0.4426077992236841, 1.7298590276177421, 0.3080138791226903, -0.7041412082298373},
        EloCase{{{0, 3, 10, 5, 2}},
                52.49112967044888, 66.49054926047955, 0.9442410565319299, 87.42081659113526},
        EloCase{{{50, 400, 1000, 450, 60}},
                6.204866471299001, 6.235842323937144, 0.9744626991857153, 10.825692474877238});

    const auto e = elo_estimate(c.results);
    CHECK_THAT(e.elo, WithinRel(c.elo, 1e-9));
    CHECK_THAT(e.ci95_half, WithinRel(c.elo95, 1e-9));
    CHECK_THAT(e.los, WithinRel(c.los, 1e-9));
    CHECK_THAT(e.nelo, WithinRel(c.nelo, 1e-9));
    CHECK(e.ci_low < e.elo);
    CHECK(e.elo < e.ci_high);
}

TEST_CASE("symmetric distribution has zero Elo") {
    const auto e = elo_estimate(Pentanomial{{5, 5, 5, 5, 5}});
    CHECK_THAT(e.elo, WithinAbs(0, 1e-12));
    CHECK_THAT(e.nelo, WithinAbs(0, 1e-12));
    CHECK_THAT(e.los, WithinAbs(0.5, 1e-12));
    CHECK_THAT(e.ci95_half, WithinRel(98.88906720326875, 1e-9));
}

TEST_CASE("phi_inv inverts phi") {
    CHECK_THAT(phi_inv(0.975), WithinAbs(1.959963984540054, 1e-12));
    CHECK_THAT(phi(phi_inv(0.1)), WithinAbs(0.1, 1e-14));
}
