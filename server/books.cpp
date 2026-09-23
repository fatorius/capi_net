#include "server/books.hpp"

#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace capi::server {

namespace fs = std::filesystem;

namespace {

bool is_counter(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Chama fn(fen) para cada posição válida do book, em ordem.
void for_each_position(const fs::path& book, const std::function<void(std::string&&)>& fn) {
    std::ifstream in(book);
    if (!in) throw std::runtime_error("cannot open book: " + book.string());
    std::string line;
    while (std::getline(in, line)) {
        if (auto fen = epd_to_fen(line)) fn(std::move(*fen));
    }
    if (in.bad()) throw std::runtime_error("error reading book: " + book.string());
}

// SplitMix64: gerador pequeno e com saída idêntica em qualquer plataforma
// (std::uniform_int_distribution difere entre libstdc++ e libc++).
struct SplitMix64 {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
};

}  // namespace

std::optional<std::string> epd_to_fen(const std::string& line) {
    std::istringstream ss(line);
    std::string board, side, castling, ep;
    if (!(ss >> board >> side >> castling >> ep)) return std::nullopt;
    if (board.front() == '#') return std::nullopt;
    if (side != "w" && side != "b") return std::nullopt;

    int ranks = 1;
    for (char c : board) {
        if (c == '/') ++ranks;
        else if (std::string_view("pnbrqkPNBRQK12345678").find(c) == std::string_view::npos) {
            return std::nullopt;
        }
    }
    if (ranks != 8) return std::nullopt;

    std::string halfmove, fullmove;
    std::string fen = board + ' ' + side + ' ' + castling + ' ' + ep;
    if ((ss >> halfmove >> fullmove) && is_counter(halfmove) && is_counter(fullmove)) {
        return fen + ' ' + halfmove + ' ' + fullmove;
    }
    return fen + " 0 1";
}

std::size_t count_book_positions(const fs::path& book) {
    static std::mutex mutex;
    static std::map<fs::path, std::tuple<std::uintmax_t, fs::file_time_type, std::size_t>> cache;

    const auto size = fs::file_size(book);
    const auto mtime = fs::last_write_time(book);
    {
        std::lock_guard lock(mutex);
        auto it = cache.find(book);
        if (it != cache.end() && std::get<0>(it->second) == size &&
            std::get<1>(it->second) == mtime) {
            return std::get<2>(it->second);
        }
    }
    std::size_t n = 0;
    for_each_position(book, [&](std::string&&) { ++n; });
    std::lock_guard lock(mutex);
    cache[book] = {size, mtime, n};
    return n;
}

std::vector<std::string> sample_book(const fs::path& book, std::size_t k, std::uint64_t seed) {
    // Reservoir sampling (algoritmo R): uma passada, memória O(k).
    std::vector<std::string> reservoir;
    reservoir.reserve(k);
    SplitMix64 rng{seed};
    std::uint64_t seen = 0;
    for_each_position(book, [&](std::string&& fen) {
        if (reservoir.size() < k) {
            reservoir.push_back(std::move(fen));
        } else {
            const std::uint64_t j = rng.next() % (seen + 1);
            if (j < k) reservoir[j] = std::move(fen);
        }
        ++seen;
    });
    return reservoir;
}

}  // namespace capi::server
