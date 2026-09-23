#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace capi::server {

// Converte uma linha de book (EPD ou FEN) em FEN completa:
// "<board> <side> <castling> <ep> [halfmove fullmove | opcodes EPD...]".
// Opcodes EPD (bm, c0, id...) são descartados; contadores ausentes viram "0 1".
// nullopt para linha vazia, comentário (#) ou malformada.
std::optional<std::string> epd_to_fen(const std::string& line);

// Número de posições válidas do book. Resultado em cache por (caminho,
// tamanho, mtime): books grandes (milhões de linhas) são lidos uma vez.
std::size_t count_book_positions(const std::filesystem::path& book);

// Amostra `k` posições distintas (por linha) do book, determinística para um
// mesmo (book, seed). Retorna menos de k se o book tiver menos posições.
std::vector<std::string> sample_book(const std::filesystem::path& book, std::size_t k,
                                     std::uint64_t seed);

}  // namespace capi::server
