#pragma once

#include "server/config.hpp"
#include "server/types.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace capi::server {

// ---------------------------------------------------------------------------
// Registro / heartbeat de clients
// ---------------------------------------------------------------------------
struct ClientRegistration {
    std::string name;
    std::optional<std::string> hostname, os, cpu_model, cpu_arch_target;
    std::string pinning_mode;  // pinning_mode
    std::optional<std::string> core_topology, bench_ref_commit, fastchess_version;
    std::optional<int> nproc_total;
    int slots = 1;
};

struct RegisteredClient {
    std::int64_t client_id;
    bool banned;
};

// Upsert por nome. Nunca remove um banimento.
RegisteredClient register_client(pqxx::connection& conn, const ClientRegistration& reg);

// nullopt = client inexistente; senão, se está banido.
std::optional<bool> heartbeat(pqxx::connection& conn, std::int64_t client_id,
                              std::optional<double> cpu_factor);

// ---------------------------------------------------------------------------
// Teste ativo
// ---------------------------------------------------------------------------
struct ActiveTest {
    std::int64_t test_id;
    std::string kind;
    std::string candidate_commit, baseline_commit;
    int tc_base_ms, tc_increment_ms, hash_mb, threads;
    std::string book_name;
    Adjudication adjudication;
};
std::optional<ActiveTest> active_test(pqxx::connection& conn);

// ---------------------------------------------------------------------------
// Claim (§5.3)
// ---------------------------------------------------------------------------
struct ClaimedPair {
    std::int64_t pair_id;
    std::int64_t position_id;
    int position_seq;
    std::string fen;
    std::string lease_expires_at;  // ISO 8601
};

enum class ClaimStatus { Ok, UnknownClient, Banned, NoActiveTest };

struct ClaimResult {
    ClaimStatus status = ClaimStatus::Ok;
    std::int64_t test_id = 0;
    std::vector<ClaimedPair> pairs;  // pode vir vazio com Ok (fila do teste vazia)
};

// Entrega até min(slots_free, clients.slots) pares, atomicamente.
ClaimResult claim_pairs(pqxx::connection& conn, const ServerConfig& cfg,
                        std::int64_t client_id, int slots_free);

// ---------------------------------------------------------------------------
// Resultado (§5.5)
// ---------------------------------------------------------------------------
enum class SubmitStatus {
    Accepted,
    PairNotFound,
    UnknownClient,
    LeaseLost,         // 409 lease_lost
    AlreadyCompleted,  // 409 already_completed
    TestFinished,      // 409 test_finished
    ClientBanned,      // 409 client_banned
};

struct SubmitResult {
    SubmitStatus status;
    int valid_games = 0;
};

// Uma única transação: guarda de posse do lease, sanidade camada 1, inserção
// de games + game_pgns e recálculo de test_stats (com parada SPRT).
SubmitResult submit_result(pqxx::connection& conn, std::int64_t pair_id,
                           const PairReport& report);

// ---------------------------------------------------------------------------
// Abandono / recuperação de leases (§5.4)
// ---------------------------------------------------------------------------
// true se o par estava leased para este client e voltou para 'pending'.
bool abandon_pair(pqxx::connection& conn, std::int64_t pair_id, std::int64_t client_id);

struct LeaseRecovery {
    int requeued = 0;
    int discarded = 0;  // atingiram max_attempts → 'discarded' (too_many_attempts)
};
LeaseRecovery recover_expired_leases(pqxx::connection& conn, int max_attempts);

}  // namespace capi::server
