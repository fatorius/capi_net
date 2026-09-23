#include "server/queue.hpp"

#include "server/db/util.hpp"
#include "server/gzip.hpp"
#include "server/log.hpp"
#include "server/sanity.hpp"
#include "server/test_lifecycle.hpp"

#include <set>

namespace capi::server {

using db::opt;

// ---------------------------------------------------------------------------
// Clients
// ---------------------------------------------------------------------------
RegisteredClient register_client(pqxx::connection& conn, const ClientRegistration& reg) {
    pqxx::work tx{conn};
    const auto r = tx.exec(R"(
        INSERT INTO clients (name, hostname, os, cpu_model, cpu_arch_target, pinning_mode,
                             core_topology, bench_ref_commit, nproc_total, slots,
                             fastchess_version)
        VALUES ($1, $2, $3, $4, $5, $6::pinning_mode, $7, $8, $9, $10, $11)
        ON CONFLICT (name) DO UPDATE SET
            hostname = EXCLUDED.hostname, os = EXCLUDED.os, cpu_model = EXCLUDED.cpu_model,
            cpu_arch_target = EXCLUDED.cpu_arch_target, pinning_mode = EXCLUDED.pinning_mode,
            core_topology = EXCLUDED.core_topology, bench_ref_commit = EXCLUDED.bench_ref_commit,
            nproc_total = EXCLUDED.nproc_total, slots = EXCLUDED.slots,
            fastchess_version = EXCLUDED.fastchess_version,
            status = CASE WHEN clients.status = 'banned' THEN clients.status
                          ELSE 'active'::client_status END,
            last_seen = now()
        RETURNING id, status = 'banned')",
                           pqxx::params{reg.name, reg.hostname, reg.os, reg.cpu_model,
                                        reg.cpu_arch_target, reg.pinning_mode, reg.core_topology,
                                        reg.bench_ref_commit, reg.nproc_total, reg.slots,
                                        reg.fastchess_version});
    tx.commit();
    return {r[0][0].as<std::int64_t>(), r[0][1].as<bool>()};
}

std::optional<bool> heartbeat(pqxx::connection& conn, std::int64_t client_id,
                              std::optional<double> cpu_factor) {
    pqxx::work tx{conn};
    const auto r = tx.exec(R"(
        UPDATE clients SET
            last_seen = now(),
            cpu_factor = COALESCE($2, cpu_factor),
            status = CASE WHEN status = 'banned' THEN status ELSE 'active'::client_status END
        WHERE id = $1
        RETURNING status = 'banned')",
                           pqxx::params{client_id, cpu_factor});
    tx.commit();
    if (r.empty()) return std::nullopt;
    return r[0][0].as<bool>();
}

// ---------------------------------------------------------------------------
// Teste ativo
// ---------------------------------------------------------------------------
std::optional<ActiveTest> active_test(pqxx::connection& conn) {
    pqxx::read_transaction tx{conn};
    const auto r = tx.exec(R"(
        SELECT id, kind::text, candidate_commit, baseline_commit,
               tc_base_ms, tc_increment_ms, hash_mb, threads, book_name,
               adj_draw_movenumber, adj_draw_movecount, adj_draw_score_cp,
               adj_resign_movecount, adj_resign_score_cp, adj_resign_twosided
        FROM tests WHERE status = 'running')");
    if (r.empty()) return std::nullopt;
    const auto row = r[0];
    ActiveTest t{row[0].as<std::int64_t>(), row[1].as<std::string>(),
                 row[2].as<std::string>(),  row[3].as<std::string>(),
                 row[4].as<int>(),          row[5].as<int>(),
                 row[6].as<int>(),          row[7].as<int>(),
                 row[8].as<std::string>(),  {}};
    if (!row[9].is_null()) {
        t.adjudication.draw = DrawAdjudication{row[9].as<int>(), row[10].as<int>(), row[11].as<int>()};
    }
    if (!row[12].is_null()) {
        t.adjudication.resign =
            ResignAdjudication{row[12].as<int>(), row[13].as<int>(), row[14].as<bool>()};
    }
    return t;
}

// ---------------------------------------------------------------------------
// Claim
// ---------------------------------------------------------------------------
ClaimResult claim_pairs(pqxx::connection& conn, const ServerConfig& cfg,
                        std::int64_t client_id, int slots_free) {
    pqxx::work tx{conn};

    // Teste ativo, client não banido e par disponível verificados na MESMA
    // transação do claim (§5.3). CTE vazia zera o CROSS JOIN.
    // Duração estimada do par: 2 partidas × 2 lados × (base + 60 × inc),
    // escalada pelo cpu_factor do client.
    const auto r = tx.exec(R"(
        WITH active AS (
            SELECT id, 4.0 * (tc_base_ms + 60 * tc_increment_ms) AS est_pair_ms
            FROM tests WHERE status = 'running' LIMIT 1
        ),
        caller AS (
            SELECT id, COALESCE(cpu_factor, 1.0) AS cpu_factor, slots
            FROM clients WHERE id = $1 AND status <> 'banned'
        ),
        picked AS (
            SELECT jp.id
            FROM job_pairs jp, active a, caller c
            WHERE jp.test_id = a.id AND jp.status = 'pending'
            ORDER BY jp.dispatch_order
            LIMIT LEAST($2::int, (SELECT slots FROM caller))
            FOR UPDATE OF jp SKIP LOCKED
        ),
        upd AS (
            UPDATE job_pairs jp
            SET status = 'leased',
                leased_to = $1,
                leased_at = now(),
                lease_expires_at = now() + make_interval(
                    secs => GREATEST($3::double precision,
                                     6 * a.est_pair_ms * c.cpu_factor / 1000.0)),
                attempts = jp.attempts + 1
            FROM picked, active a, caller c
            WHERE jp.id = picked.id
            RETURNING jp.id, jp.position_id, jp.test_id, jp.lease_expires_at
        )
        SELECT upd.id, upd.position_id, upd.test_id, op.seq, op.fen,
               to_json(upd.lease_expires_at) #>> '{}'
        FROM upd JOIN opening_positions op ON op.id = upd.position_id
        ORDER BY upd.id)",
                           pqxx::params{client_id, std::max(slots_free, 0),
                                        static_cast<double>(cfg.lease_ttl_min.count())});

    ClaimResult out;
    if (!r.empty()) {
        out.test_id = r[0][2].as<std::int64_t>();
        for (const auto& row : r) {
            out.pairs.push_back({row[0].as<std::int64_t>(), row[1].as<std::int64_t>(),
                                 row[3].as<int>(), row[4].as<std::string>(),
                                 row[5].as<std::string>()});
        }
        tx.commit();
        return out;
    }

    // Nada entregue: leitura só para montar a resposta HTTP correta.
    const auto d = tx.exec(R"(
        SELECT (SELECT status::text FROM clients WHERE id = $1),
               (SELECT id FROM tests WHERE status = 'running'))",
                           pqxx::params{client_id});
    tx.commit();
    const auto client_status = opt<std::string>(d[0][0]);
    const auto test_id = opt<std::int64_t>(d[0][1]);
    if (!client_status) out.status = ClaimStatus::UnknownClient;
    else if (*client_status == "banned") out.status = ClaimStatus::Banned;
    else if (!test_id) out.status = ClaimStatus::NoActiveTest;
    else out.test_id = *test_id;
    return out;
}

// ---------------------------------------------------------------------------
// Resultado
// ---------------------------------------------------------------------------
SubmitResult submit_result(pqxx::connection& conn, std::int64_t pair_id,
                           const PairReport& report) {
    pqxx::work tx{conn};

    // Trava o par: a decisão sobre a posse do lease e a gravação acontecem
    // sob o mesmo lock, então um resultado atrasado nunca sobrescreve outro.
    const auto g = tx.exec(R"(
        SELECT jp.status::text, jp.leased_to, jp.test_id, t.status::text, c.status::text
        FROM job_pairs jp
        JOIN tests t ON t.id = jp.test_id
        LEFT JOIN clients c ON c.id = $2
        WHERE jp.id = $1
        FOR UPDATE OF jp)",
                           pqxx::params{pair_id, report.client_id});
    if (g.empty()) return {SubmitStatus::PairNotFound};

    const auto pair_status = g[0][0].as<std::string>();
    const auto leased_to = opt<std::int64_t>(g[0][1]);
    const auto test_id = g[0][2].as<std::int64_t>();
    const auto test_status = g[0][3].as<std::string>();
    const auto client_status = opt<std::string>(g[0][4]);
    const bool mine = leased_to && *leased_to == report.client_id;

    if (!client_status) return {SubmitStatus::UnknownClient};
    if (*client_status == "banned") return {SubmitStatus::ClientBanned};
    if (pair_status == "completed" && mine) return {SubmitStatus::AlreadyCompleted};
    if (test_status != "running") return {SubmitStatus::TestFinished};
    if (pair_status != "leased" || !mine) return {SubmitStatus::LeaseLost};

    tx.exec(R"(
        UPDATE job_pairs SET status = 'completed', completed_at = now()
        WHERE id = $1 AND status = 'leased' AND leased_to = $2
        RETURNING id)",
            pqxx::params{pair_id, report.client_id})
        .expect_rows(1);

    const auto verdicts = check_pair(report);
    int valid_games = 0;
    for (int i = 0; i < 2; ++i) {
        const auto& game = report.games[i];
        const auto& v = verdicts[i];
        if (v.valid) ++valid_games;

        const auto game_id = tx.query_value<std::int64_t>(
            R"(
            INSERT INTO games (pair_id, test_id, client_id, slot_index, core_id, game_in_pair,
                               candidate_is_white, outcome, termination, ply_count, duration_ms,
                               cpu_factor_at_play, tc_base_effective_ms,
                               tc_increment_effective_ms, valid, invalid_reason)
            VALUES ($1, $2, $3, $4, $5, $6, $7, $8::game_outcome, $9::game_termination,
                    $10, $11, $12, $13, $14, $15, $16)
            RETURNING id)",
            pqxx::params{pair_id, test_id, report.client_id, game.slot_index, game.core_id,
                         game.game_in_pair, game.candidate_is_white, game.outcome,
                         game.termination, game.ply_count, game.duration_ms,
                         game.cpu_factor_at_play, game.tc_base_effective_ms,
                         game.tc_increment_effective_ms, v.valid,
                         v.valid ? std::optional<std::string>{} : v.reason});

        const auto gz = gzip_compress(game.pgn);
        tx.exec("INSERT INTO game_pgns (game_id, pgn_gz) VALUES ($1, $2)",
                pqxx::params{game_id, pqxx::binary_cast(gz)});
    }

    refresh_test(tx, test_id);
    tx.commit();
    return {SubmitStatus::Accepted, valid_games};
}

// ---------------------------------------------------------------------------
// Abandono / recuperação de leases
// ---------------------------------------------------------------------------
bool abandon_pair(pqxx::connection& conn, std::int64_t pair_id, std::int64_t client_id) {
    pqxx::work tx{conn};
    const auto r = tx.exec(R"(
        UPDATE job_pairs
        SET status = 'pending', leased_to = NULL, leased_at = NULL, lease_expires_at = NULL
        WHERE id = $1 AND status = 'leased' AND leased_to = $2)",
                           pqxx::params{pair_id, client_id});
    tx.commit();
    return r.affected_rows() == 1;
}

LeaseRecovery recover_expired_leases(pqxx::connection& conn, int max_attempts) {
    pqxx::work tx{conn};
    // SKIP LOCKED: pares travados por um envio de resultado em curso ficam de
    // fora; o próximo ciclo os reavalia.
    const auto r = tx.exec(R"(
        WITH expired AS (
            SELECT id FROM job_pairs
            WHERE status = 'leased' AND lease_expires_at < now()
            FOR UPDATE SKIP LOCKED
        )
        UPDATE job_pairs jp SET
            status = CASE WHEN jp.attempts >= $1 THEN 'discarded'::job_status
                          ELSE 'pending'::job_status END,
            discarded_reason = CASE WHEN jp.attempts >= $1 THEN 'too_many_attempts' END,
            leased_to        = CASE WHEN jp.attempts >= $1 THEN jp.leased_to END,
            leased_at        = CASE WHEN jp.attempts >= $1 THEN jp.leased_at END,
            lease_expires_at = CASE WHEN jp.attempts >= $1 THEN jp.lease_expires_at END
        FROM expired
        WHERE jp.id = expired.id
        RETURNING jp.id, jp.test_id, jp.position_id, jp.attempts, jp.status = 'discarded')",
                           pqxx::params{max_attempts});

    LeaseRecovery out;
    std::set<std::int64_t> tests_with_discards;
    for (const auto& row : r) {
        if (row[4].as<bool>()) {
            ++out.discarded;
            tests_with_discards.insert(row[1].as<std::int64_t>());
            log_warn("ALERT pair " + row[0].as<std::string>() + " (test " +
                     row[1].as<std::string>() + ", position " + row[2].as<std::string>() +
                     ") discarded after " + row[3].as<std::string>() +
                     " attempts: too_many_attempts");
        } else {
            ++out.requeued;
        }
    }
    // Um descarte pode ter consumido o último par pendente do teste.
    for (auto test_id : tests_with_discards) refresh_test(tx, test_id);
    tx.commit();
    return out;
}

}  // namespace capi::server
