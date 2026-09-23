#include "server/spec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace capi::server;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {
json base_spec() {
    return {{"kind", "sprt"},       {"candidate_ref", "feature/lmr"}, {"baseline_ref", "v2.1.0"},
            {"tc", "10+0.1"},       {"book_name", "UHO_Lichess_4852_v1.epd"},
            {"total_pairs", 15000}, {"preset", "gainer"},             {"name", "LMR tweak"}};
}

std::string error_of(const json& j) {
    try {
        parse_test_spec(j, {});
    } catch (const SpecError& e) {
        return e.what();
    }
    return "";
}
}  // namespace

TEST_CASE("parse_tc") {
    CHECK(parse_tc("10+0.1").base_ms == 10000);
    CHECK(parse_tc("10+0.1").increment_ms == 100);
    CHECK(parse_tc("60").increment_ms == 0);
    CHECK(parse_tc("0.5+0.01").base_ms == 500);
    CHECK(parse_tc("8+0.08").increment_ms == 80);
    CHECK_THROWS_AS(parse_tc(""), SpecError);
    CHECK_THROWS_AS(parse_tc("0+1"), SpecError);
    CHECK_THROWS_AS(parse_tc("10+"), SpecError);
    CHECK_THROWS_AS(parse_tc("-10+1"), SpecError);
    CHECK_THROWS_AS(parse_tc("10+0.1+1"), SpecError);
    CHECK_THROWS_AS(parse_tc("1e3+1"), SpecError);
    CHECK_THROWS_AS(parse_tc("40/60"), SpecError);
}

TEST_CASE("valid_ref and valid_book_name") {
    for (const char* ok : {"main", "feature/lmr", "v2.1.0", "a6bd4ac", "fix_eval-2"}) {
        CHECK(valid_ref(ok));
    }
    for (const char* bad : {"", "-rf", "a..b", "/x", "x/", "a b", "x;y", "$(id)", "a\"b"}) {
        CHECK_FALSE(valid_ref(bad));
    }
    CHECK(valid_book_name("UHO_Lichess_4852_v1.epd"));
    for (const char* bad : {"", "../etc/passwd", "a/b.epd", ".hidden", "x y.epd"}) {
        CHECK_FALSE(valid_book_name(bad));
    }
}

TEST_CASE("gainer preset fills normalized bounds") {
    const auto s = parse_test_spec(base_spec(), {});
    CHECK(s.kind == "sprt");
    CHECK(s.tc_base_ms == 10000);
    CHECK(s.hash_mb == 16);
    CHECK(*s.sprt_elo0 == 0.0);
    CHECK(*s.sprt_elo1 == 5.0);
    CHECK(*s.sprt_alpha == 0.05);
    CHECK(s.priority == 0);
}

TEST_CASE("nonreg preset and custom bounds") {
    auto j = base_spec();
    j["preset"] = "nonreg";
    const auto nonreg = parse_test_spec(j, {});
    CHECK(*nonreg.sprt_elo0 == -5.0);
    CHECK(*nonreg.sprt_elo1 == 0.0);

    j["preset"] = "custom";
    j["sprt_elo0"] = -1.5;
    j["sprt_elo1"] = 2;
    j["sprt_beta"] = 0.1;
    const auto custom = parse_test_spec(j, {});
    CHECK(*custom.sprt_elo0 == -1.5);
    CHECK(*custom.sprt_elo1 == 2.0);
    CHECK(*custom.sprt_alpha == 0.05);
    CHECK(*custom.sprt_beta == 0.1);
}

TEST_CASE("gauntlet has no SPRT parameters") {
    auto j = base_spec();
    j["kind"] = "gauntlet";
    j.erase("preset");
    const auto s = parse_test_spec(j, {});
    CHECK_FALSE(s.sprt_preset);
    CHECK_FALSE(s.sprt_elo0);

    j["preset"] = "gainer";
    CHECK_THAT(error_of(j), ContainsSubstring("gauntlet"));
}

TEST_CASE("default name") {
    auto j = base_spec();
    j.erase("name");
    CHECK(parse_test_spec(j, {}).name == "feature/lmr vs v2.1.0");
}

TEST_CASE("invalid specs are rejected with a reason") {
    auto j = base_spec();
    j.erase("preset");
    CHECK_THAT(error_of(j), ContainsSubstring("preset"));

    j = base_spec();
    j["preset"] = "custom";
    CHECK_THAT(error_of(j), ContainsSubstring("sprt_elo0"));
    j["sprt_elo0"] = 5;
    j["sprt_elo1"] = 0;
    CHECK_THAT(error_of(j), ContainsSubstring("< sprt_elo1"));
    j["sprt_elo0"] = 0;
    j["sprt_elo1"] = 5;
    j["sprt_alpha"] = 1.0;
    CHECK_THAT(error_of(j), ContainsSubstring("(0, 1)"));

    j = base_spec();
    j["sprt_elo0"] = 1;  // bounds só com custom
    CHECK_THAT(error_of(j), ContainsSubstring("custom"));

    j = base_spec();
    j["kind"] = "match";
    CHECK_THAT(error_of(j), ContainsSubstring("kind"));

    j = base_spec();
    j["total_pairs"] = 0;
    CHECK_THAT(error_of(j), ContainsSubstring("total_pairs"));
    j["total_pairs"] = "100";
    CHECK_THAT(error_of(j), ContainsSubstring("integer"));

    j = base_spec();
    j["book_name"] = "../../etc/passwd";
    CHECK_THAT(error_of(j), ContainsSubstring("book_name"));

    j = base_spec();
    j["candidate_ref"] = "--upload-pack=x";
    CHECK_THAT(error_of(j), ContainsSubstring("git ref"));

    j = base_spec();
    j["hash_mb"] = 0;
    CHECK_THAT(error_of(j), ContainsSubstring("hash_mb"));

    CHECK_THAT(error_of(json::array()), ContainsSubstring("object"));
}
