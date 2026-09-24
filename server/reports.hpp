#pragma once

#include <pqxx/pqxx>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace capi::server {

// Pontos do gráfico LLR × pares (test_stats_history), reduzidos a no máximo
// `max_points` (mantém o primeiro, o último e uma amostra uniforme).
struct HistoryPoint {
    int pairs;
    std::optional<double> llr, elo;
    std::string at;
};
std::vector<HistoryPoint> stats_history(pqxx::connection& conn, std::int64_t test_id,
                                        std::size_t max_points);

// Clients que jogaram (ou estão jogando) o teste.
struct TestClient {
    std::int64_t client_id;
    std::string name, status, pinning_mode;
    std::optional<std::string> cpu_model, core_topology, arch_target, fastchess_version;
    int slots;
    std::optional<double> cpu_factor;
    int pairs_completed, games_invalid, pairs_leased;
    std::string last_seen;
    bool online;  // heartbeat nos últimos 2 minutos
    std::optional<double> nps;  // nós/s das duas engines nas partidas deste teste
};
std::vector<TestClient> test_clients(pqxx::connection& conn, std::int64_t test_id);

// Todos os clients, online primeiro, com o NPS das últimas `recent_games`
// partidas (de qualquer teste) medido nas próprias partidas.
struct ClientSummary {
    std::int64_t client_id;
    std::string name, status, pinning_mode;
    std::optional<std::string> cpu_model, core_topology, arch_target, fastchess_version, os;
    int slots;
    std::string last_seen;
    bool online;
    int pairs_leased;               // em jogo agora
    std::int64_t games_total;
    std::optional<double> nps;      // média das últimas `recent_games` partidas
    int nps_games;                  // quantas partidas entraram na média
    std::optional<std::string> last_game_at;
};
std::vector<ClientSummary> all_clients(pqxx::connection& conn, int recent_games);

// Velocidade das engines no teste (partidas válidas com contagem de nós).
struct TestSpeed {
    std::optional<double> candidate_nps, baseline_nps;
    std::int64_t games = 0;
};
TestSpeed test_speed(pqxx::connection& conn, std::int64_t test_id);

// PGN (descomprimido) de uma partida do teste.
std::optional<std::string> game_pgn(pqxx::connection& conn, std::int64_t test_id,
                                    std::int64_t game_id);

// Próximo lote de PGNs comprimidos do teste, com id > after_game_id, em ordem.
struct PgnChunk {
    std::int64_t game_id;
    std::string pgn_gz;
};
std::vector<PgnChunk> pgn_batch(pqxx::connection& conn, std::int64_t test_id,
                                std::int64_t after_game_id, int limit);

bool test_exists(pqxx::connection& conn, std::int64_t test_id);

}  // namespace capi::server
