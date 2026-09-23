#pragma once

#include "server/types.hpp"

#include <array>
#include <string>

namespace capi::server {

struct GameVerdict {
    bool valid = true;
    std::string reason;  // vazio se válida
};

// Sanidade camada 1 (plano §6) — subconjunto atual: terminação, contagem de
// lances e inversão de cores do par. Parse de PGN, consistência do resultado
// com o PGN e faixa de duração entram no passo 6 do §13.
std::array<GameVerdict, 2> check_pair(const PairReport& report);

inline constexpr int kMaxPlyCount = 2000;

}  // namespace capi::server
