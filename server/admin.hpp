#pragma once

#include "server/spec.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace capi::server {

// Grava o teste com os SHAs congelados, em 'validating' (§5.1 passo 3).
std::int64_t insert_test(pqxx::connection& conn, const TestSpec& spec,
                         const std::string& candidate_commit, const std::string& baseline_commit);

// Parada manual (§5.8 `capi_net stop`). Se o teste estava 'running', promove
// o próximo da fila na mesma transação.
enum class StopStatus { Stopped, NotFound, AlreadyTerminal };
struct StopOutcome {
    StopStatus status;
    std::string previous_status;
    std::optional<std::int64_t> promoted;
};
StopOutcome stop_test(pqxx::connection& conn, std::int64_t test_id);

// Só testes ainda não iniciados ('validating' ou 'queued').
enum class PriorityStatus { Updated, NotFound, NotQueued };
struct PriorityOutcome {
    PriorityStatus status;
    std::string test_status;
};
PriorityOutcome set_priority(pqxx::connection& conn, std::int64_t test_id, int priority);

struct ValidationInfo {
    std::string status;
    std::optional<std::string> log;
};
std::optional<ValidationInfo> get_validation(pqxx::connection& conn, std::int64_t test_id);

struct TestSummary {
    std::int64_t id;
    std::string name, kind, status, result;
    std::string candidate_ref, candidate_commit, baseline_ref, baseline_commit;
    int priority, total_pairs, pairs_valid;
    std::optional<double> llr, llr_lower, llr_upper, elo;
    std::string created_at;
};
// Ordem: running, depois queued/validating (fila), depois o resto (mais novos primeiro).
std::vector<TestSummary> list_tests(pqxx::connection& conn, int limit);

}  // namespace capi::server
