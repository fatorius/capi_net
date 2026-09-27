#pragma once

#include "server/types.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace capi::server {

struct GameVerdict {
    bool valid = true;
    std::string reason;  // vazio se válida
};

// Sanidade camada 1 (plano §6) — subconjunto atual: terminação, contagem de
// lances, duração plausível e inversão de cores do par. Parse de PGN e
// consistência do resultado com o PGN ainda não.
std::array<GameVerdict, 2> check_pair(const PairReport& report);

inline constexpr int kMaxPlyCount = 2000;

// Duração máxima plausível de uma partida: os dois relógios somados
// (2 × base + incremento × plies), com folga de 25% + 5 s para latência do
// harness e para o arredondamento da duração a segundos (tag GameDuration).
// Acima disso o relógio de parede não mediu só a partida — ex: o host dormiu
// no meio do jogo e um lado perdeu por tempo sem ter pensado.
std::int64_t max_plausible_duration_ms(int tc_base_ms, int tc_increment_ms, int ply_count);

}  // namespace capi::server
