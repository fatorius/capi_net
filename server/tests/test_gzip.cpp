#include "server/gzip.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

using namespace capi::server;

TEST_CASE("gzip round-trips PGN text") {
    std::string pgn = "[Event \"capi_net\"]\n[Result \"1-0\"]\n\n";
    for (int i = 1; i <= 2000; ++i) pgn += std::to_string(i) + ". e4 e5 ";
    pgn += "1-0\n";

    const auto gz = gzip_compress(pgn);
    CHECK(gz.size() < pgn.size() / 4);
    CHECK(static_cast<unsigned char>(gz[0]) == 0x1f);  // magic gzip
    CHECK(static_cast<unsigned char>(gz[1]) == 0x8b);
    CHECK(gzip_decompress(gz) == pgn);
}

TEST_CASE("gzip handles empty input") {
    CHECK(gzip_decompress(gzip_compress("")).empty());
}

TEST_CASE("gzip rejects corrupt and truncated data") {
    const auto gz = gzip_compress("some pgn text some pgn text some pgn text");
    CHECK_THROWS_AS(gzip_decompress("not gzip at all"), std::runtime_error);
    CHECK_THROWS_AS(gzip_decompress(gz.substr(0, gz.size() / 2)), std::runtime_error);
}
