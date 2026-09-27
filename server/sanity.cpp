#include "server/sanity.hpp"

namespace capi::server {

std::int64_t max_plausible_duration_ms(int tc_base_ms, int tc_increment_ms, int ply_count) {
    const std::int64_t clocks =
        2LL * tc_base_ms + static_cast<std::int64_t>(tc_increment_ms) * ply_count;
    return clocks + clocks / 4 + 5000;
}

namespace {

GameVerdict check_game(const GameReport& g) {
    if (g.termination == "crash" || g.termination == "illegal_move") {
        return {false, "termination_" + g.termination};
    }
    if (!g.ply_count || *g.ply_count <= 0 || *g.ply_count > kMaxPlyCount) {
        return {false, "ply_count_out_of_range"};
    }
    if (!g.duration_ms || *g.duration_ms < 0) return {false, "duration_missing"};
    if (*g.duration_ms > max_plausible_duration_ms(g.tc_base_effective_ms,
                                                    g.tc_increment_effective_ms, *g.ply_count)) {
        return {false, "duration_implausible"};
    }
    return {};
}

}  // namespace

std::array<GameVerdict, 2> check_pair(const PairReport& report) {
    std::array<GameVerdict, 2> v{check_game(report.games[0]), check_game(report.games[1])};

    if (report.games[0].candidate_is_white == report.games[1].candidate_is_white) {
        for (auto& verdict : v) {
            if (verdict.valid) verdict = {false, "pair_colors_not_inverted"};
        }
    }
    return v;
}

}  // namespace capi::server
