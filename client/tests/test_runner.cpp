#include "client/runner.hpp"

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

EngineSet engines() {
    EngineSet e;
    e.test_id = 7;
    e.candidate = "/w/cand/capizero";
    e.baseline = "/w/base/capizero";
    e.tc_base_ms = 10000;
    e.tc_increment_ms = 100;
    e.hash_mb = 16;
    e.threads = 1;
    return e;
}
}  // namespace

TEST_CASE("effective TC scales with cpu_factor and formats for fastchess") {
    const auto e = engines();
    const auto tc1 = effective_tc(e, 1.0);
    CHECK(fastchess_tc(tc1) == "10+0.1");
    const auto slow = effective_tc(e, 1.049456);  // exemplo do plano §3.2
    CHECK(slow.base_ms == 10495);
    CHECK(slow.increment_ms == 105);
    CHECK(fastchess_tc(slow) == "10.495+0.105");
    CHECK(fastchess_tc({60000, 0}) == "60+0");
}

TEST_CASE("fastchess command plays one pair, sequentially, without -concurrency N") {
    const auto args = fastchess_args("fastchess", engines(), {10000, 100}, "/s/pair.epd",
                                     "/s/pair.pgn", "ev");
    auto has = [&](const std::string& a) {
        return std::find(args.begin(), args.end(), a) != args.end();
    };
    CHECK(args.front() == "fastchess");
    CHECK(has("cmd=/w/cand/capizero"));
    CHECK(has("name=candidate"));
    CHECK(has("tc=10+0.1"));
    CHECK(has("option.Hash=16"));
    CHECK(has("option.Threads=1"));
    CHECK(has("file=/s/pair.epd"));
    CHECK(has("-repeat"));
    const auto conc = std::find(args.begin(), args.end(), "-concurrency");
    REQUIRE(conc != args.end());
    CHECK(*(conc + 1) == "1");
    const auto rounds = std::find(args.begin(), args.end(), "-rounds");
    CHECK(*(rounds + 1) == "1");
    CHECK_FALSE(has("-use-affinity"));  // a afinidade é do client, não do fastchess
}

TEST_CASE("adjudication rules become fastchess flags") {
    auto e = engines();
    e.adjudication.draw = DrawAdjudication{34, 8, 20};
    e.adjudication.resign = ResignAdjudication{5, 1000, true};
    const auto args = fastchess_args("fastchess", e, {10000, 100}, "a.epd", "a.pgn", "ev");
    auto after = [&](const std::string& flag) {
        auto it = std::find(args.begin(), args.end(), flag);
        REQUIRE(it != args.end());
        return std::vector<std::string>(it + 1, it + 4);
    };
    CHECK(after("-draw") == std::vector<std::string>{"movenumber=34", "movecount=8", "score=20"});
    CHECK(after("-resign") ==
          std::vector<std::string>{"movecount=5", "score=1000", "twosided=true"});

    const auto none = fastchess_args("fastchess", engines(), {10000, 100}, "a.epd", "a.pgn", "ev");
    CHECK(std::find(none.begin(), none.end(), "-draw") == none.end());
    CHECK(std::find(none.begin(), none.end(), "-resign") == none.end());
}

TEST_CASE("result body from a real fastchess pair") {
    const SlotInfo slot{1, 3};
    const auto body = make_result_body(42, fixture("fastchess_pair.pgn"), "", slot,
                                       {10000, 100}, 1.0);
    CHECK(body["client_id"] == 42);
    const auto& g = body["games"];
    REQUIRE(g.size() == 2);
    CHECK(g[0]["game_in_pair"] == 0);
    CHECK(g[0]["candidate_is_white"] == true);
    CHECK(g[1]["candidate_is_white"] == false);
    CHECK(g[0]["outcome"] == "candidate_loss");
    CHECK(g[1]["outcome"] == "candidate_win");
    CHECK(g[0]["termination"] == "timeout");
    CHECK(g[0]["ply_count"] == 49);
    CHECK(g[0]["core_id"] == 3);
    CHECK(g[0]["slot_index"] == 1);
    CHECK(g[0]["tc_base_effective_ms"] == 10000);
    CHECK(g[0]["pgn"].get<std::string>().starts_with("[Event "));
}

TEST_CASE("missing games become crash records, never silently dropped") {
    const SlotInfo slot{0, std::nullopt};
    const auto none = make_result_body(1, "", "fastchess saiu com código 1.", slot, {1000, 10}, 1.0);
    for (int i = 0; i < 2; ++i) {
        CHECK(none["games"][i]["termination"] == "crash");
        CHECK(none["games"][i]["core_id"].is_null());
        CHECK(none["games"][i]["pgn"].get<std::string>().find("código 1") != std::string::npos);
    }
    CHECK(none["games"][0]["candidate_is_white"] != none["games"][1]["candidate_is_white"]);

    // Só a primeira partida chegou: a segunda fica com a cor oposta.
    const auto full = fixture("fastchess_pair.pgn");
    const auto first_only = full.substr(0, full.find("\n[Event ", 1) + 1);
    const auto half = make_result_body(1, first_only, "timeout", slot, {1000, 10}, 1.0);
    CHECK(half["games"][0]["termination"] == "timeout");
    CHECK(half["games"][1]["termination"] == "crash");
    CHECK(half["games"][1]["candidate_is_white"] == false);
}
