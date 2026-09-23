#include "server/test_lifecycle.hpp"

#include "common/elo.hpp"
#include "common/gsprt.hpp"
#include "server/db/util.hpp"
#include "server/log.hpp"

namespace capi::server {

using db::opt;
namespace st = capi::stats;

namespace {

void finish_test(pqxx::work& tx, std::int64_t test_id, const std::string& result,
                 const std::string& why) {
    tx.exec(R"(
        UPDATE tests
        SET status = 'finished', result = $2::test_result, finished_at = now()
        WHERE id = $1)",
            pqxx::params{test_id, result});
    log_info("test " + std::to_string(test_id) + " finished: " + result + " (" + why + ")");
    if (auto next = promote_next_test(tx)) {
        log_info("test " + std::to_string(*next) + " promoted to running");
    }
}

}  // namespace

void refresh_test(pqxx::work& tx, std::int64_t test_id) {
    // FOR NO KEY UPDATE (não FOR UPDATE): a transação chamadora já inseriu em
    // games/job_pairs, cujas FKs seguram FOR KEY SHARE nesta linha; FOR UPDATE
    // conflitaria com o KEY SHARE de outro envio concorrente e causaria deadlock.
    const auto t = tx.exec(R"(
        SELECT kind::text, status::text, result::text,
               sprt_elo0, sprt_elo1, sprt_alpha, sprt_beta
        FROM tests WHERE id = $1
        FOR NO KEY UPDATE)",
                           pqxx::params{test_id});
    if (t.empty()) return;
    const auto kind = t[0][0].as<std::string>();
    const auto status = t[0][1].as<std::string>();
    const auto current_result = t[0][2].as<std::string>();

    st::Pentanomial penta;
    for (const auto& row : tx.exec(R"(
            SELECT pair_score, count(*) FROM pair_outcomes
            WHERE test_id = $1 GROUP BY pair_score)",
                                   pqxx::params{test_id})) {
        penta.add(st::category_from_pair_score(row[0].as<double>()), row[1].as<std::int64_t>());
    }

    std::optional<double> elo, ci_low, ci_high, llr, llr_lower, llr_upper;
    if (penta.pairs() > 0) {
        const auto e = st::elo_estimate(penta);
        elo = e.elo;
        ci_low = e.ci_low;
        ci_high = e.ci_high;
    }

    auto decision = st::SprtDecision::Continue;
    if (kind == "sprt") {
        const double elo0 = t[0][3].as<double>();
        const double elo1 = t[0][4].as<double>();
        const double alpha = opt<double>(t[0][5]).value_or(0.05);
        const double beta = opt<double>(t[0][6]).value_or(0.05);
        const auto bounds = st::sprt_bounds(alpha, beta);
        llr = st::llr_normalized(elo0, elo1, penta);
        llr_lower = bounds.lower;
        llr_upper = bounds.upper;
        if (penta.pairs() > 0) decision = st::sprt_decide(*llr, bounds);
    }

    using P = st::Penta;
    tx.exec(R"(
        INSERT INTO test_stats (test_id, pairs_valid, penta_ll, penta_ld, penta_dd_wl,
                                penta_wd, penta_ww, llr, llr_lower, llr_upper,
                                elo, elo_ci_low, elo_ci_high, updated_at)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, now())
        ON CONFLICT (test_id) DO UPDATE SET
            pairs_valid = EXCLUDED.pairs_valid,
            penta_ll = EXCLUDED.penta_ll, penta_ld = EXCLUDED.penta_ld,
            penta_dd_wl = EXCLUDED.penta_dd_wl, penta_wd = EXCLUDED.penta_wd,
            penta_ww = EXCLUDED.penta_ww,
            llr = EXCLUDED.llr, llr_lower = EXCLUDED.llr_lower, llr_upper = EXCLUDED.llr_upper,
            elo = EXCLUDED.elo, elo_ci_low = EXCLUDED.elo_ci_low,
            elo_ci_high = EXCLUDED.elo_ci_high, updated_at = now())",
            pqxx::params{test_id, penta.pairs(), penta[P::LL], penta[P::LD], penta[P::DD_WL],
                         penta[P::WD], penta[P::WW], llr, llr_lower, llr_upper, elo, ci_low,
                         ci_high});

    // Histórico para o gráfico LLR × pares: só quando o número de pares muda.
    tx.exec(R"(
        INSERT INTO test_stats_history (test_id, pairs_valid, llr, elo)
        SELECT $1::bigint, $2::int, $3::double precision, $4::double precision
        WHERE $2::int IS DISTINCT FROM (SELECT pairs_valid FROM test_stats_history
                                   WHERE test_id = $1 ORDER BY id DESC LIMIT 1))",
            pqxx::params{test_id, penta.pairs(), llr, elo});

    if (status != "running") return;

    if (decision == st::SprtDecision::AcceptH1) {
        finish_test(tx, test_id, "accepted", "LLR crossed upper bound");
        return;
    }
    if (decision == st::SprtDecision::AcceptH0) {
        finish_test(tx, test_id, "rejected", "LLR crossed lower bound");
        return;
    }

    const auto remaining = tx.query_value<std::int64_t>(
        "SELECT count(*) FROM job_pairs "
        "WHERE test_id = $1 AND status IN ('pending', 'leased')",
        pqxx::params{test_id});
    if (remaining == 0) {
        // Gauntlet não tem veredito: mantém o result atual ('pending').
        finish_test(tx, test_id, kind == "sprt" ? "inconclusive" : current_result,
                    "all pairs consumed");
    }
}

std::optional<std::int64_t> promote_next_test(pqxx::work& tx) {
    tx.exec("SELECT pg_advisory_xact_lock(hashtext('capi_net.promote'))");
    const auto r = tx.exec(R"(
        UPDATE tests SET status = 'running', started_at = now()
        WHERE id = (SELECT id FROM tests WHERE status = 'queued'
                    ORDER BY priority DESC, created_at, id
                    LIMIT 1)
          AND NOT EXISTS (SELECT 1 FROM tests WHERE status = 'running')
        RETURNING id)");
    if (r.empty()) return std::nullopt;
    return r[0][0].as<std::int64_t>();
}

std::optional<std::int64_t> running_test_id(pqxx::work& tx) {
    const auto r = tx.exec("SELECT id FROM tests WHERE status = 'running'");
    if (r.empty()) return std::nullopt;
    return r[0][0].as<std::int64_t>();
}

std::optional<TestStatsSnapshot> load_test_stats(pqxx::connection& conn, std::int64_t test_id) {
    pqxx::read_transaction tx{conn};
    const auto r = tx.exec(R"(
        SELECT t.id, t.name, t.kind::text, t.status::text, t.result::text, t.total_pairs,
               c.pending, c.leased, c.completed, c.discarded,
               s.test_id IS NOT NULL AS has_stats,
               s.penta_ll, s.penta_ld, s.penta_dd_wl, s.penta_wd, s.penta_ww,
               s.llr, s.llr_lower, s.llr_upper, s.elo, s.elo_ci_low, s.elo_ci_high,
               t.sprt_elo0, t.sprt_elo1, t.sprt_alpha, t.sprt_beta,
               to_json(s.updated_at) #>> '{}',
               t.candidate_ref, t.candidate_commit, t.baseline_ref, t.baseline_commit,
               t.tc_base_ms, t.tc_increment_ms, t.hash_mb, t.threads, t.priority, t.book_name,
               to_json(t.created_at) #>> '{}', to_json(t.started_at) #>> '{}',
               to_json(t.finished_at) #>> '{}',
               t.adj_draw_movenumber, t.adj_draw_movecount, t.adj_draw_score_cp,
               t.adj_resign_movecount, t.adj_resign_score_cp, t.adj_resign_twosided
        FROM tests t
        CROSS JOIN LATERAL (
            SELECT count(*) FILTER (WHERE status = 'pending')   AS pending,
                   count(*) FILTER (WHERE status = 'leased')    AS leased,
                   count(*) FILTER (WHERE status = 'completed') AS completed,
                   count(*) FILTER (WHERE status = 'discarded') AS discarded
            FROM job_pairs WHERE test_id = t.id) c
        LEFT JOIN test_stats s ON s.test_id = t.id
        WHERE t.id = $1)",
                           pqxx::params{test_id});
    if (r.empty()) return std::nullopt;
    const auto row = r[0];

    TestStatsSnapshot s{};
    s.test_id = row[0].as<std::int64_t>();
    s.name = row[1].as<std::string>();
    s.kind = row[2].as<std::string>();
    s.status = row[3].as<std::string>();
    s.result = row[4].as<std::string>();
    s.total_pairs = row[5].as<int>();
    s.pairs_pending = row[6].as<int>();
    s.pairs_leased = row[7].as<int>();
    s.pairs_completed = row[8].as<int>();
    s.pairs_discarded = row[9].as<int>();
    s.has_stats = row[10].as<bool>();
    if (s.has_stats) {
        for (int i = 0; i < 5; ++i) s.penta.counts[i] = row[11 + i].as<std::int64_t>();
    }
    s.llr = opt<double>(row[16]);
    s.llr_lower = opt<double>(row[17]);
    s.llr_upper = opt<double>(row[18]);
    s.elo = opt<double>(row[19]);
    s.elo_ci_low = opt<double>(row[20]);
    s.elo_ci_high = opt<double>(row[21]);
    s.sprt_elo0 = opt<double>(row[22]);
    s.sprt_elo1 = opt<double>(row[23]);
    s.sprt_alpha = opt<double>(row[24]);
    s.sprt_beta = opt<double>(row[25]);
    s.stats_updated_at = opt<std::string>(row[26]);
    s.candidate_ref = row[27].as<std::string>();
    s.candidate_commit = row[28].as<std::string>();
    s.baseline_ref = row[29].as<std::string>();
    s.baseline_commit = row[30].as<std::string>();
    s.tc_base_ms = row[31].as<int>();
    s.tc_increment_ms = row[32].as<int>();
    s.hash_mb = row[33].as<int>();
    s.threads = row[34].as<int>();
    s.priority = row[35].as<int>();
    s.book_name = row[36].as<std::string>();
    s.created_at = row[37].as<std::string>();
    s.started_at = opt<std::string>(row[38]);
    s.finished_at = opt<std::string>(row[39]);
    if (!row[40].is_null()) {
        s.adjudication.draw =
            DrawAdjudication{row[40].as<int>(), row[41].as<int>(), row[42].as<int>()};
    }
    if (!row[43].is_null()) {
        s.adjudication.resign =
            ResignAdjudication{row[43].as<int>(), row[44].as<int>(), row[45].as<bool>()};
    }
    return s;
}

}  // namespace capi::server
