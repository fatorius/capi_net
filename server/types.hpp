#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace capi::server {

// Regras de adjudicação de um teste (colunas adj_* de tests). nullopt = desligada.
struct DrawAdjudication {
    int movenumber, movecount, score_cp;
};
struct ResignAdjudication {
    int movecount, score_cp;
    bool twosided;
};
struct Adjudication {
    std::optional<DrawAdjudication> draw;
    std::optional<ResignAdjudication> resign;
};

// Uma partida reportada pelo client (plano §9, POST /api/jobs/{id}/result).
// outcome/termination já foram validados contra os enums do banco.
struct GameReport {
    int game_in_pair = 0;           // 0 ou 1
    bool candidate_is_white = false;
    std::string outcome;            // game_outcome
    std::string termination;        // game_termination
    std::optional<int> ply_count;
    std::optional<int> duration_ms;
    double cpu_factor_at_play = 1.0;
    int tc_base_effective_ms = 0;
    int tc_increment_effective_ms = 0;
    int slot_index = 0;
    std::optional<int> core_id;
    std::string pgn;                // texto puro; o server comprime
};

// Par completo: games[i].game_in_pair == i.
struct PairReport {
    std::int64_t client_id = 0;
    std::array<GameReport, 2> games;
};

}  // namespace capi::server
