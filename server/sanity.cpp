#include "server/sanity.hpp"

namespace capi::server {

namespace {

GameVerdict check_game(const GameReport& g) {
    if (g.termination == "crash" || g.termination == "illegal_move") {
        return {false, "termination_" + g.termination};
    }
    if (!g.ply_count || *g.ply_count <= 0 || *g.ply_count > kMaxPlyCount) {
        return {false, "ply_count_out_of_range"};
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
