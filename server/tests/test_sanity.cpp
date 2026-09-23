#include "server/sanity.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace capi::server;

namespace {
PairReport good_pair() {
    PairReport r;
    r.client_id = 1;
    for (int i = 0; i < 2; ++i) {
        auto& g = r.games[i];
        g.game_in_pair = i;
        g.candidate_is_white = (i == 0);
        g.outcome = "draw";
        g.termination = "normal";
        g.ply_count = 80;
        g.duration_ms = 20000;
    }
    return r;
}
}  // namespace

TEST_CASE("well-formed pair is valid") {
    const auto v = check_pair(good_pair());
    CHECK(v[0].valid);
    CHECK(v[1].valid);
}

TEST_CASE("crash and illegal move invalidate only that game") {
    auto r = good_pair();
    r.games[1].termination = "crash";
    auto v = check_pair(r);
    CHECK(v[0].valid);
    CHECK_FALSE(v[1].valid);
    CHECK(v[1].reason == "termination_crash");

    r.games[1].termination = "illegal_move";
    v = check_pair(r);
    CHECK(v[1].reason == "termination_illegal_move");
}

TEST_CASE("ply count must be present and in range") {
    auto r = good_pair();
    r.games[0].ply_count.reset();
    r.games[1].ply_count = kMaxPlyCount + 1;
    const auto v = check_pair(r);
    CHECK(v[0].reason == "ply_count_out_of_range");
    CHECK(v[1].reason == "ply_count_out_of_range");
}

TEST_CASE("colors must be inverted within the pair") {
    auto r = good_pair();
    r.games[1].candidate_is_white = true;
    r.games[1].termination = "timeout";  // continua válida isoladamente
    const auto v = check_pair(r);
    CHECK(v[0].reason == "pair_colors_not_inverted");
    CHECK(v[1].reason == "pair_colors_not_inverted");
}
