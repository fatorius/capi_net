#include "server/admin.hpp"

#include "server/db/util.hpp"
#include "server/test_lifecycle.hpp"

namespace capi::server {

using db::opt;

std::int64_t insert_test(pqxx::connection& conn, const TestSpec& s,
                         const std::string& candidate_commit, const std::string& baseline_commit) {
    pqxx::work tx{conn};
    const auto id = tx.query_value<std::int64_t>(
        R"(
        INSERT INTO tests (name, kind, status, candidate_ref, candidate_commit,
                           baseline_ref, baseline_commit, tc_base_ms, tc_increment_ms,
                           hash_mb, threads, book_name, total_pairs,
                           sprt_preset, sprt_elo0, sprt_elo1, sprt_alpha, sprt_beta,
                           priority, submitted_by)
        VALUES ($1, $2::test_kind, 'validating', $3, $4, $5, $6, $7, $8, $9, 1, $10, $11,
                $12::sprt_preset, $13, $14, $15, $16, $17, $18)
        RETURNING id)",
        pqxx::params{s.name, s.kind, s.candidate_ref, candidate_commit, s.baseline_ref,
                     baseline_commit, s.tc_base_ms, s.tc_increment_ms, s.hash_mb, s.book_name,
                     s.total_pairs, s.sprt_preset, s.sprt_elo0, s.sprt_elo1, s.sprt_alpha,
                     s.sprt_beta, s.priority, s.submitted_by});
    tx.commit();
    return id;
}

StopOutcome stop_test(pqxx::connection& conn, std::int64_t test_id) {
    pqxx::work tx{conn};
    const auto r = tx.exec("SELECT status::text FROM tests WHERE id = $1 FOR NO KEY UPDATE",
                           pqxx::params{test_id});
    if (r.empty()) return {StopStatus::NotFound, "", std::nullopt};
    const auto prev = r[0][0].as<std::string>();
    if (prev != "validating" && prev != "queued" && prev != "running") {
        return {StopStatus::AlreadyTerminal, prev, std::nullopt};
    }
    tx.exec("UPDATE tests SET status = 'stopped', finished_at = now() WHERE id = $1",
            pqxx::params{test_id});
    std::optional<std::int64_t> promoted;
    if (prev == "running") promoted = promote_next_test(tx);
    tx.commit();
    return {StopStatus::Stopped, prev, promoted};
}

PriorityOutcome set_priority(pqxx::connection& conn, std::int64_t test_id, int priority) {
    pqxx::work tx{conn};
    const auto r = tx.exec("SELECT status::text FROM tests WHERE id = $1 FOR NO KEY UPDATE",
                           pqxx::params{test_id});
    if (r.empty()) return {PriorityStatus::NotFound, ""};
    const auto status = r[0][0].as<std::string>();
    if (status != "validating" && status != "queued") return {PriorityStatus::NotQueued, status};
    tx.exec("UPDATE tests SET priority = $2 WHERE id = $1", pqxx::params{test_id, priority});
    tx.commit();
    return {PriorityStatus::Updated, status};
}

std::optional<ValidationInfo> get_validation(pqxx::connection& conn, std::int64_t test_id) {
    pqxx::read_transaction tx{conn};
    const auto r = tx.exec("SELECT status::text, validation_log FROM tests WHERE id = $1",
                           pqxx::params{test_id});
    if (r.empty()) return std::nullopt;
    return ValidationInfo{r[0][0].as<std::string>(), opt<std::string>(r[0][1])};
}

std::vector<TestSummary> list_tests(pqxx::connection& conn, int limit) {
    pqxx::read_transaction tx{conn};
    const auto r = tx.exec(R"(
        SELECT t.id, t.name, t.kind::text, t.status::text, t.result::text,
               t.candidate_ref, t.candidate_commit, t.baseline_ref, t.baseline_commit,
               t.priority, t.total_pairs, COALESCE(s.pairs_valid, 0),
               s.llr, s.llr_lower, s.llr_upper, s.elo,
               to_json(t.created_at) #>> '{}'
        FROM tests t
        LEFT JOIN test_stats s ON s.test_id = t.id
        ORDER BY CASE t.status WHEN 'running' THEN 0
                               WHEN 'queued' THEN 1
                               WHEN 'validating' THEN 1
                               ELSE 2 END,
                 CASE WHEN t.status IN ('queued', 'validating') THEN -t.priority END,
                 CASE WHEN t.status IN ('queued', 'validating') THEN t.id END,
                 t.id DESC
        LIMIT $1)",
                           pqxx::params{limit});
    std::vector<TestSummary> out;
    for (const auto& row : r) {
        out.push_back({row[0].as<std::int64_t>(), row[1].as<std::string>(),
                       row[2].as<std::string>(), row[3].as<std::string>(),
                       row[4].as<std::string>(), row[5].as<std::string>(),
                       row[6].as<std::string>(), row[7].as<std::string>(),
                       row[8].as<std::string>(), row[9].as<int>(), row[10].as<int>(),
                       row[11].as<int>(), opt<double>(row[12]), opt<double>(row[13]),
                       opt<double>(row[14]), opt<double>(row[15]), row[16].as<std::string>()});
    }
    return out;
}

}  // namespace capi::server
