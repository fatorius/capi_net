#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace capi::client {

// Uma partida do PGN gerado pelo fastchess, já traduzida para o vocabulário
// do server (enums game_outcome / game_termination).
struct ParsedGame {
    std::map<std::string, std::string> tags;
    std::string text;              // PGN desta partida (tags + lances)
    bool candidate_is_white = false;
    std::string outcome;           // candidate_win | draw | candidate_loss
    std::string termination;       // normal | timeout | illegal_move | crash | adjudication | other
    std::optional<int> ply_count;
    std::optional<int> duration_ms;
};

// Separa o PGN em partidas (na ordem do arquivo) e extrai as tags.
std::vector<std::map<std::string, std::string>> pgn_tags(const std::string& pgn);
std::vector<std::string> split_pgn_games(const std::string& pgn);

// Interpreta uma partida em que o candidato se chama `candidate_name`.
// nullopt se o candidato não jogou a partida ou o resultado for '*'.
std::optional<ParsedGame> parse_game(const std::string& game_pgn, const std::string& candidate_name);

// "time forfeit" -> "timeout", etc. Termination ausente = "normal".
std::string map_termination(const std::string& tag);

// "HH:MM:SS" (tag GameDuration) -> ms.
std::optional<int> parse_duration(const std::string& hhmmss);

}  // namespace capi::client
