#include "common/penta.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <stdexcept>

using namespace capi::stats;
using Catch::Matchers::WithinAbs;

TEST_CASE("pair score maps to pentanomial category") {
    CHECK(category_from_pair_score(0.0) == Penta::LL);
    CHECK(category_from_pair_score(0.5) == Penta::LD);
    CHECK(category_from_pair_score(1.0) == Penta::DD_WL);
    CHECK(category_from_pair_score(1.5) == Penta::WD);
    CHECK(category_from_pair_score(2.0) == Penta::WW);
    CHECK_THROWS_AS(category_from_pair_score(0.25), std::invalid_argument);
    CHECK_THROWS_AS(category_from_pair_score(2.5), std::invalid_argument);
    CHECK_THROWS_AS(category_from_pair_score(-0.5), std::invalid_argument);
}

TEST_CASE("pentanomial accumulates and sums") {
    Pentanomial p;
    for (double s : {0.0, 1.0, 1.0, 2.0, 1.5}) p.add_pair_score(s);
    CHECK(p.pairs() == 5);
    CHECK(p[Penta::DD_WL] == 2);
    CHECK(p[Penta::LD] == 0);

    Pentanomial q{{1, 1, 1, 1, 1}};
    p += q;
    CHECK(p.pairs() == 10);
    CHECK(p[Penta::DD_WL] == 3);
}

TEST_CASE("pdf regularizes zero counts") {
    const auto pdf = to_pdf(Pentanomial{{0, 3, 10, 5, 2}});
    CHECK_THAT(pdf.count, WithinAbs(20.001, 1e-12));
    CHECK_THAT(pdf.probs[0], WithinAbs(1e-3 / 20.001, 1e-15));
    CHECK_THAT(pdf.values[3], WithinAbs(0.75, 0));
}
