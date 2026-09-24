#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace capi::client {

// Nós e tempo de busca somados de uma engine numa partida. NPS = nodes / time.
struct EngineUsage {
    long long nodes = 0;
    long long time_ms = 0;
};

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
    // Somas dos comentários por lance do fastchess (-pgnout nodes=true):
    // {+0.58/10 0.154s, n=686046, nps=...}. nullopt se o PGN não tem nós.
    std::optional<EngineUsage> candidate_usage, baseline_usage;
};

// Separa o PGN em partidas (na ordem do arquivo) e extrai as tags.
std::vector<std::map<std::string, std::string>> pgn_tags(const std::string& pgn);
std::vector<std::string> split_pgn_games(const std::string& pgn);

// Interpreta uma partida em que o candidato se chama `candidate_name`.
// nullopt se o candidato não jogou a partida ou o resultado for '*'.
std::optional<ParsedGame> parse_game(const std::string& game_pgn, const std::string& candidate_name);

// Soma nós e tempo por cor a partir dos comentários dos lances. `white_first`
// indica quem joga o primeiro lance (lado a mover na FEN inicial).
struct ColorUsage {
    std::optional<EngineUsage> white, black;
};
ColorUsage usage_by_color(const std::string& game_pgn, bool white_first);

// "time forfeit" -> "timeout", etc. Termination ausente = "normal".
std::string map_termination(const std::string& tag);

// "HH:MM:SS" (tag GameDuration) -> ms.
std::optional<int> parse_duration(const std::string& hhmmss);

}  // namespace capi::client
