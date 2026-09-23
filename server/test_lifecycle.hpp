#pragma once

#include "common/penta.hpp"
#include "server/types.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <optional>
#include <string>

namespace capi::server {

// Recalcula test_stats do zero a partir de pair_outcomes (§5.6) e aplica as
// condições de parada (§5.7): fronteira SPRT ou esgotamento dos pares.
// Chamar dentro da transação que alterou os dados do teste: a função trava a
// linha de `tests`, o que serializa recálculos concorrentes do mesmo teste.
void refresh_test(pqxx::work& tx, std::int64_t test_id);

// Promove o 'queued' de maior prioridade (desempate: mais antigo) para
// 'running' se não houver teste 'running'. Serializado por advisory lock.
std::optional<std::int64_t> promote_next_test(pqxx::work& tx);

// Id do teste 'running', se houver.
std::optional<std::int64_t> running_test_id(pqxx::work& tx);

struct TestStatsSnapshot {
    std::int64_t test_id;
    std::string name, kind, status, result;
    int total_pairs;
    int pairs_pending, pairs_leased, pairs_completed, pairs_discarded;
    bool has_stats;
    capi::stats::Pentanomial penta;
    std::optional<double> llr, llr_lower, llr_upper, elo, elo_ci_low, elo_ci_high;
    std::optional<double> sprt_elo0, sprt_elo1, sprt_alpha, sprt_beta;
    std::optional<std::string> stats_updated_at;

    std::string candidate_ref, candidate_commit, baseline_ref, baseline_commit;
    int tc_base_ms, tc_increment_ms, hash_mb, threads, priority;
    std::string book_name;
    std::string created_at;
    std::optional<std::string> started_at, finished_at;
    Adjudication adjudication;
};

std::optional<TestStatsSnapshot> load_test_stats(pqxx::connection& conn, std::int64_t test_id);

}  // namespace capi::server
