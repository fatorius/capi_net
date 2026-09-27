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
        g.tc_base_effective_ms = 10000;
        g.tc_increment_effective_ms = 100;
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

TEST_CASE("duration must fit within both clocks plus slack") {
    // 10+0.1, 80 plies: relógios somam 28 s -> limite 28 + 7 + 5 = 40 s.
    CHECK(max_plausible_duration_ms(10000, 100, 80) == 40000);

    auto r = good_pair();
    r.games[0].duration_ms = 40000;  // no limite: válida
    r.games[1].duration_ms = 0;      // mate rápido arredondado para 0 s: válida
    auto v = check_pair(r);
    CHECK(v[0].valid);
    CHECK(v[1].valid);

    // Casos reais do teste 3 (Mac dormindo no meio da partida):
    r.games[0].duration_ms = 208000;  // 80 plies em 208 s
    r.games[1].ply_count = 3;
    r.games[1].duration_ms = 189000;  // 3 plies em 189 s
    v = check_pair(r);
    CHECK(v[0].reason == "duration_implausible");
    CHECK(v[1].reason == "duration_implausible");

    r = good_pair();
    r.games[1].duration_ms.reset();
    CHECK(check_pair(r)[1].reason == "duration_missing");
}

TEST_CASE("duration limit scales with the effective time control") {
    // Client lento (cpu_factor 3): 30+0.3 com 200 plies -> 120 s de relógio.
    CHECK(max_plausible_duration_ms(30000, 300, 200) == 120000 + 30000 + 5000);
    auto r = good_pair();
    for (auto& g : r.games) {
        g.tc_base_effective_ms = 30000;
        g.tc_increment_effective_ms = 300;
        g.ply_count = 200;
        g.duration_ms = 110000;
    }
    const auto v = check_pair(r);
    CHECK(v[0].valid);
    CHECK(v[1].valid);
}
