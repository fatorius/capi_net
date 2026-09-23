#include "common/gsprt.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace capi::stats;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
struct LlrCase {
    Pentanomial results;
    double elo0, elo1, expected;
};
}  // namespace

// Valores de referência gerados executando LLRcalc.LLR_normalized do próprio
// Fishtest (server/fishtest/stats/LLRcalc.py, master em 2026-09).
TEST_CASE("llr_normalized matches Fishtest reference") {
    const auto c = GENERATE(
        LlrCase{{{10789, 19328, 33806, 19402, 10543}}, 0, 5, -26.838448639795093},
        LlrCase{{{10789, 19328, 33806, 19402, 10543}}, -5, 0, 12.039751536817539},
        LlrCase{{{10789, 19328, 33806, 19402, 10543}}, 0.764, 3.439, -12.701291983706488},
        LlrCase{{{10789, 19328, 33806, 19402, 10543}}, -3, 1, 0.3008697854813657},
        LlrCase{{{39, 2226, 31451, 2412, 40}}, 0, 5, 2.847898594083},
        LlrCase{{{39, 2226, 31451, 2412, 40}}, -5, 0, 17.832223595824992},
        LlrCase{{{39, 2226, 31451, 2412, 40}}, 0.764, 3.439, 2.1629408695845367},
        LlrCase{{{39, 2226, 31451, 2412, 40}}, -3, 1, 10.667041287007896},
        LlrCase{{{1187, 7410, 13475, 7378, 1164}}, 0, 5, -8.12581361915957},
        LlrCase{{{1187, 7410, 13475, 7378, 1164}}, -5, 0, 4.554450117505817},
        LlrCase{{{0, 3, 10, 5, 2}}, 0, 5, 0.14646048082087207},
        LlrCase{{{0, 3, 10, 5, 2}}, -3, 1, 0.1215767130479021},
        LlrCase{{{50, 400, 1000, 450, 60}}, 0, 5, 1.3501528635944866},
        LlrCase{{{50, 400, 1000, 450, 60}}, -5, 0, 2.1611931334828456},
        LlrCase{{{5, 5, 5, 5, 5}}, 0, 5, -0.005176938008656367},
        LlrCase{{{5, 5, 5, 5, 5}}, -3, 1, 0.0016567539493761486});

    INFO("elo0=" << c.elo0 << " elo1=" << c.elo1 << " expected=" << c.expected);
    CHECK_THAT(llr_normalized(c.elo0, c.elo1, c.results), WithinRel(c.expected, 1e-6));
}

TEST_CASE("llr_normalized is antisymmetric for a symmetric distribution") {
    const Pentanomial sym{{7, 40, 100, 40, 7}};
    CHECK_THAT(llr_normalized(-2, 3, sym), WithinAbs(-llr_normalized(-3, 2, sym), 1e-9));
}

TEST_CASE("llr_normalized with no pairs is zero") {
    CHECK(llr_normalized(0, 5, Pentanomial{}) == 0.0);
}

TEST_CASE("sprt bounds and decision") {
    const auto b = sprt_bounds(0.05, 0.05);
    CHECK_THAT(b.upper, WithinAbs(std::log(19.0), 1e-12));   // 2.944
    CHECK_THAT(b.lower, WithinAbs(-std::log(19.0), 1e-12));  // -2.944
    CHECK(sprt_decide(0.0, b) == SprtDecision::Continue);
    CHECK(sprt_decide(3.0, b) == SprtDecision::AcceptH1);
    CHECK(sprt_decide(-3.0, b) == SprtDecision::AcceptH0);
}
