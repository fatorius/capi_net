#include "client/topology.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace capi::client;
using Catch::Matchers::ContainsSubstring;

namespace {
Topology make(bool hetero, bool pinning, std::vector<int> fast, int nproc,
              std::string desc = "desc") {
    Topology t;
    t.heterogeneous = hetero;
    t.pinning_available = pinning;
    t.fast_cores = std::move(fast);
    t.nproc = nproc;
    t.description = std::move(desc);
    return t;
}
}  // namespace

TEST_CASE("§3.3: heterogeneous with pinning pins on fast cores") {
    const auto t = make(true, true, {0, 2, 4, 6, 8, 10, 12, 14}, 20, "8P+4E");
    const auto a = decide_admission(t, 3, std::nullopt);
    REQUIRE(a.ok);
    CHECK(a.pinning_mode == "pinned");
    CHECK(a.cores == std::vector<int>{10, 12, 14});
    CHECK_THAT(a.core_topology, ContainsSubstring("8P+4E"));
    CHECK_THAT(a.core_topology, ContainsSubstring("10,12,14"));
}

TEST_CASE("§3.3: homogeneous with pinning still pins") {
    const auto a = decide_admission(make(false, true, {0, 1, 2, 3}, 4), 1, std::nullopt);
    REQUIRE(a.ok);
    CHECK(a.pinning_mode == "pinned");
    CHECK(a.cores == std::vector<int>{3});
}

TEST_CASE("§3.3: homogeneous without pinning is allowed unpinned") {
    const auto a = decide_admission(make(false, false, {0, 1, 2, 3}, 8), 2, std::nullopt);
    REQUIRE(a.ok);
    CHECK(a.pinning_mode == "homogeneous_unpinned");
    CHECK(a.cores.empty());
}

TEST_CASE("§3.3: heterogeneous without pinning or QoS preference is refused") {
    const auto a = decide_admission(make(true, false, {0, 1, 2, 3, 4}, 11, "5P+6E"), 1,
                                    std::nullopt);
    CHECK_FALSE(a.ok);
    CHECK_THAT(a.error, ContainsSubstring("5P+6E"));
    CHECK_THAT(a.error, ContainsSubstring("§3.3"));
}

TEST_CASE("§12 degraded mode: Apple Silicon admitted as qos_hint with P-2 slots") {
    auto t = make(true, false, {0, 1, 2, 3, 4}, 11, "5P+6E");  // M3 Pro
    t.qos_hint_available = true;
    const auto a = decide_admission(t, 3, std::nullopt);
    REQUIRE(a.ok);
    CHECK(a.pinning_mode == "qos_hint");
    CHECK(a.cores.empty());
    CHECK_THAT(a.core_topology, ContainsSubstring("qos_hint"));

    const auto too_many = decide_admission(t, 4, std::nullopt);
    CHECK_FALSE(too_many.ok);
    CHECK_THAT(too_many.error, ContainsSubstring("no máximo 3"));
    CHECK_FALSE(decide_admission(t, 1, std::vector<int>{0}).ok);  // sem afinidade

    CHECK(default_slots(t) == 2);  // NPROC/4 = 2 ≤ 3

    auto m1 = make(true, false, {0, 1, 2, 3}, 8, "4P+4E");  // M1: 4 P-cores
    m1.qos_hint_available = true;
    CHECK(decide_admission(m1, 2, std::nullopt).ok);
    CHECK_FALSE(decide_admission(m1, 3, std::nullopt).ok);
}

TEST_CASE("slots cannot exceed fast physical cores") {
    CHECK_FALSE(decide_admission(make(true, true, {0, 1}, 8), 3, std::nullopt).ok);
    CHECK_FALSE(decide_admission(make(false, false, {0, 1}, 4), 3, std::nullopt).ok);
    CHECK_FALSE(decide_admission(make(false, true, {0, 1}, 4), 0, std::nullopt).ok);
}

TEST_CASE("explicit cores must be fast, distinct and one per slot") {
    const auto t = make(true, true, {0, 2, 4, 6}, 12);
    const auto ok = decide_admission(t, 2, std::vector<int>{2, 6});
    REQUIRE(ok.ok);
    CHECK(ok.cores == std::vector<int>{2, 6});

    CHECK_THAT(decide_admission(t, 2, std::vector<int>{2, 3}).error, ContainsSubstring("3"));
    CHECK_FALSE(decide_admission(t, 2, std::vector<int>{2, 2}).ok);
    CHECK_FALSE(decide_admission(t, 2, std::vector<int>{2}).ok);
    CHECK_FALSE(decide_admission(make(false, false, {0, 1}, 2), 1, std::vector<int>{0}).ok);
}

TEST_CASE("default slots is NPROC/4, at least 1") {
    CHECK(default_slots(make(false, true, {0, 1, 2, 3}, 4)) == 1);
    CHECK(default_slots(make(false, true, {}, 16)) == 4);
    CHECK(default_slots(make(false, true, {}, 2)) == 1);
}

TEST_CASE("this host is detected consistently") {
    const auto t = detect_topology();
    CHECK(t.nproc >= 1);
    CHECK_FALSE(t.fast_cores.empty());
    CHECK_FALSE(t.description.empty());
    CHECK(static_cast<int>(t.fast_cores.size()) <= t.nproc);
}
