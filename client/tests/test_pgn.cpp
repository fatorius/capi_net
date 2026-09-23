#include "client/pgn.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

using namespace capi::client;

namespace {
std::string fixture(const std::string& name) {
    std::ifstream in(std::string(CAPI_CLIENT_TEST_DIR) + "/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

// PGNs reais do fastchess 1.8.0 (capizero main vs beta2.8.0).
TEST_CASE("time forfeit pair: colors, outcome and termination") {
    const auto games = split_pgn_games(fixture("fastchess_pair.pgn"));
    REQUIRE(games.size() == 2);

    const auto g0 = parse_game(games[0], "candidate");
    REQUIRE(g0);
    CHECK(g0->candidate_is_white);
    CHECK(g0->outcome == "candidate_loss");  // 0-1, candidato de brancas
    CHECK(g0->termination == "timeout");
    CHECK(g0->ply_count == 49);
    CHECK(g0->duration_ms == 2000);
    CHECK(g0->text.starts_with("[Event "));
    CHECK(g0->text.find("[White \"baseline\"]") == std::string::npos);  // só esta partida

    const auto g1 = parse_game(games[1], "candidate");
    REQUIRE(g1);
    CHECK_FALSE(g1->candidate_is_white);
    CHECK(g1->outcome == "candidate_win");  // 0-1, candidato de pretas
    CHECK(g1->tags.at("FEN") == g0->tags.at("FEN"));
}

TEST_CASE("normal terminations: mate and repetition") {
    const auto games = split_pgn_games(fixture("fastchess_two_pairs.pgn"));
    REQUIRE(games.size() == 4);
    std::vector<std::string> outcomes;
    for (const auto& text : games) {
        const auto g = parse_game(text, "candidate");
        REQUIRE(g);
        CHECK(g->termination == "normal");
        outcomes.push_back(g->outcome);
    }
    CHECK(outcomes == std::vector<std::string>{"candidate_win", "candidate_loss", "draw",
                                               "candidate_loss"});
}

TEST_CASE("unknown player or unfinished game is rejected") {
    const auto games = split_pgn_games(fixture("fastchess_pair.pgn"));
    CHECK_FALSE(parse_game(games[0], "someone_else"));
    std::string unfinished = games[0];
    unfinished.replace(unfinished.find("[Result \"0-1\"]"), 14, "[Result \"*\"]");
    CHECK_FALSE(parse_game(unfinished, "candidate"));
}

TEST_CASE("termination mapping") {
    CHECK(map_termination("") == "normal");
    CHECK(map_termination("normal") == "normal");
    CHECK(map_termination("time forfeit") == "timeout");
    CHECK(map_termination("illegal move") == "illegal_move");
    CHECK(map_termination("adjudication") == "adjudication");
    CHECK(map_termination("abandoned") == "crash");
    CHECK(map_termination("stalled connection") == "crash");
    CHECK(map_termination("something new") == "other");
}

TEST_CASE("duration parsing") {
    CHECK(parse_duration("00:00:02") == 2000);
    CHECK(parse_duration("01:02:03") == 3723000);
    CHECK_FALSE(parse_duration("2s"));
}

TEST_CASE("empty or garbage PGN yields no games") {
    CHECK(split_pgn_games("").empty());
    CHECK(split_pgn_games("garbage\n").empty());
}
