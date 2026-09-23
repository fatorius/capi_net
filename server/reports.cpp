#include "server/reports.hpp"

#include "server/db/util.hpp"
#include "server/gzip.hpp"

namespace capi::server {

using db::opt;

std::vector<HistoryPoint> stats_history(pqxx::connection& conn, std::int64_t test_id,
                                        std::size_t max_points) {
    pqxx::read_transaction tx{conn};
    std::vector<HistoryPoint> all;
    for (const auto& row : tx.exec(R"(
            SELECT pairs_valid, llr, elo, to_json(recorded_at) #>> '{}'
            FROM test_stats_history WHERE test_id = $1 ORDER BY id)",
                                   pqxx::params{test_id})) {
        all.push_back({row[0].as<int>(), opt<double>(row[1]), opt<double>(row[2]),
                       row[3].as<std::string>()});
    }
    if (all.size() <= max_points || max_points < 2) return all;

    std::vector<HistoryPoint> out;
    const double step = static_cast<double>(all.size() - 1) / static_cast<double>(max_points - 1);
    for (std::size_t i = 0; i < max_points; ++i) {
        out.push_back(all[static_cast<std::size_t>(i * step + 0.5)]);
    }
    out.back() = all.back();
    return out;
}

std::vector<TestClient> test_clients(pqxx::connection& conn, std::int64_t test_id) {
    pqxx::read_transaction tx{conn};
    const auto r = tx.exec(R"(
        WITH played AS (
            SELECT client_id,
                   count(DISTINCT pair_id)             AS pairs,
                   count(*) FILTER (WHERE NOT valid)   AS invalid
            FROM games WHERE test_id = $1 GROUP BY client_id
        ),
        leased AS (
            SELECT leased_to AS client_id, count(*) AS n
            FROM job_pairs WHERE test_id = $1 AND status = 'leased' GROUP BY leased_to
        )
        SELECT c.id, c.name, c.status::text, c.pinning_mode::text, c.cpu_model,
               c.core_topology, c.cpu_arch_target, c.fastchess_version, c.slots, c.cpu_factor,
               COALESCE(p.pairs, 0), COALESCE(p.invalid, 0), COALESCE(l.n, 0),
               to_json(c.last_seen) #>> '{}', c.last_seen > now() - interval '2 minutes'
        FROM clients c
        LEFT JOIN played p ON p.client_id = c.id
        LEFT JOIN leased l ON l.client_id = c.id
        WHERE p.client_id IS NOT NULL OR l.client_id IS NOT NULL
        ORDER BY COALESCE(p.pairs, 0) DESC, c.name)",
                           pqxx::params{test_id});
    std::vector<TestClient> out;
    for (const auto& row : r) {
        out.push_back({row[0].as<std::int64_t>(), row[1].as<std::string>(),
                       row[2].as<std::string>(), row[3].as<std::string>(),
                       opt<std::string>(row[4]), opt<std::string>(row[5]),
                       opt<std::string>(row[6]), opt<std::string>(row[7]), row[8].as<int>(),
                       opt<double>(row[9]), row[10].as<int>(), row[11].as<int>(),
                       row[12].as<int>(), row[13].as<std::string>(), row[14].as<bool>()});
    }
    return out;
}

std::optional<std::string> game_pgn(pqxx::connection& conn, std::int64_t test_id,
                                    std::int64_t game_id) {
    pqxx::read_transaction tx{conn};
    const auto r = tx.exec(R"(
        SELECT p.pgn_gz FROM game_pgns p JOIN games g ON g.id = p.game_id
        WHERE g.id = $1 AND g.test_id = $2)",
                           pqxx::params{game_id, test_id});
    if (r.empty()) return std::nullopt;
    const auto bytes = r[0][0].as<pqxx::bytes>();
    return gzip_decompress(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::vector<PgnChunk> pgn_batch(pqxx::connection& conn, std::int64_t test_id,
                                std::int64_t after_game_id, int limit) {
    pqxx::read_transaction tx{conn};
    std::vector<PgnChunk> out;
    for (const auto& row : tx.exec(R"(
            SELECT g.id, p.pgn_gz FROM games g JOIN game_pgns p ON p.game_id = g.id
            WHERE g.test_id = $1 AND g.id > $2
            ORDER BY g.id LIMIT $3)",
                                   pqxx::params{test_id, after_game_id, limit})) {
        const auto bytes = row[1].as<pqxx::bytes>();
        out.push_back({row[0].as<std::int64_t>(),
                       std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())});
    }
    return out;
}

bool test_exists(pqxx::connection& conn, std::int64_t test_id) {
    pqxx::read_transaction tx{conn};
    return !tx.exec("SELECT 1 FROM tests WHERE id = $1", pqxx::params{test_id}).empty();
}

}  // namespace capi::server
