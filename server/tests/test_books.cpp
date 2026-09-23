#include "server/books.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <unistd.h>

using namespace capi::server;
namespace fs = std::filesystem;

TEST_CASE("epd_to_fen normalizes EPD and FEN lines") {
    const std::string board = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR";
    CHECK(epd_to_fen(board + " b KQkq e3 0 1") == board + " b KQkq e3 0 1");
    CHECK(epd_to_fen(board + " b KQkq - 3 12\r") == board + " b KQkq - 3 12");
    CHECK(epd_to_fen(board + " b KQkq -") == board + " b KQkq - 0 1");
    CHECK(epd_to_fen(board + " b KQkq - bm e5; c0 \"x\";") == board + " b KQkq - 0 1");
    CHECK(epd_to_fen(board + " b KQkq - hmvc 3; fmvn 7;") == board + " b KQkq - 0 1");
    CHECK_FALSE(epd_to_fen(""));
    CHECK_FALSE(epd_to_fen("   "));
    CHECK_FALSE(epd_to_fen("# comment line with words"));
    CHECK_FALSE(epd_to_fen(board + " x KQkq -"));
    CHECK_FALSE(epd_to_fen("8/8/8 w - -"));
    CHECK_FALSE(epd_to_fen("rnbqkbnr/ppppXppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -"));
}

namespace {
struct TempBook {
    fs::path path;
    explicit TempBook(int positions) {
        path = fs::temp_directory_path() /
               ("capi_net_book_" + std::to_string(getpid()) + "_" + std::to_string(positions) +
                ".epd");
        std::ofstream out(path);
        out << "# header comment\n\n";
        for (int i = 0; i < positions; ++i) {
            // Posições distintas: variam o contador de meio-lances.
            out << "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - " << i << " 1\n";
        }
        out << "garbage line\n";
    }
    ~TempBook() { fs::remove(path); }
};
}  // namespace

TEST_CASE("count_book_positions skips invalid lines") {
    TempBook book(100);
    CHECK(count_book_positions(book.path) == 100);
    CHECK(count_book_positions(book.path) == 100);  // cache
}

TEST_CASE("sample_book is deterministic per seed and without repetition") {
    TempBook book(1000);
    const auto a = sample_book(book.path, 50, 42);
    const auto b = sample_book(book.path, 50, 42);
    const auto c = sample_book(book.path, 50, 43);
    CHECK(a.size() == 50);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(std::set<std::string>(a.begin(), a.end()).size() == 50);

    // Amostra espalhada pelo book, não só o começo do arquivo.
    int from_second_half = 0;
    for (const auto& fen : a) {
        const auto halfmove = std::stoi(fen.substr(fen.rfind(' ', fen.size() - 3) + 1));
        from_second_half += halfmove >= 500;
    }
    CHECK(from_second_half > 10);
    CHECK(from_second_half < 40);
}

TEST_CASE("sample_book returns everything when k exceeds the book") {
    TempBook book(10);
    CHECK(sample_book(book.path, 50, 1).size() == 10);
}
