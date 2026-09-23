#include "server/http/routes.hpp"

#include "server/github.hpp"
#include "server/http/helpers.hpp"
#include "server/log.hpp"
#include "server/queue.hpp"
#include "server/test_lifecycle.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <string>

namespace capi::server::http {

using nlohmann::json;

namespace {

GameReport parse_game(const json& g) {
    if (!g.is_object()) throw BadRequest("each game must be an object");
    GameReport r;
    r.game_in_pair = req_int(g, "game_in_pair", 0);
    if (r.game_in_pair > 1) throw BadRequest("game_in_pair must be 0 or 1");
    r.candidate_is_white = req_bool(g, "candidate_is_white");
    r.outcome = req_enum(g, "outcome", kOutcomes);
    r.termination = req_enum(g, "termination", kTerminations);
    r.ply_count = opt_int(g, "ply_count", 0);
    r.duration_ms = opt_int(g, "duration_ms", 0);
    r.cpu_factor_at_play = req_double(g, "cpu_factor");
    if (!(r.cpu_factor_at_play > 0)) throw BadRequest("cpu_factor must be positive");
    r.tc_base_effective_ms = req_int(g, "tc_base_effective_ms", 0);
    r.tc_increment_effective_ms = req_int(g, "tc_increment_effective_ms", 0);
    r.slot_index = req_int(g, "slot_index", 0);
    r.core_id = opt_int(g, "core_id", 0);
    r.pgn = req_string(g, "pgn");
    return r;
}

}  // namespace

void register_routes(httplib::Server& svr, db::ConnectionPool& pool, const ServerConfig& cfg) {
    svr.Get("/healthz", guarded([&](const httplib::Request&, httplib::Response& res) {
        auto conn = pool.acquire();
        pqxx::nontransaction tx{*conn};
        tx.exec("SELECT 1");
        send_json(res, 200, {{"status", "ok"}});
    }));

    // --- clients -----------------------------------------------------------
    svr.Post("/api/clients/register",
             guarded([&](const httplib::Request& req, httplib::Response& res) {
        const auto j = parse_body(req);
        ClientRegistration reg;
        reg.name = req_string(j, "name");
        if (reg.name.empty()) throw BadRequest("name must not be empty");
        reg.hostname = opt_string(j, "hostname");
        reg.os = opt_string(j, "os");
        reg.cpu_model = opt_string(j, "cpu_model");
        reg.cpu_arch_target = opt_string(j, "arch_target");
        reg.pinning_mode = req_enum(j, "pinning_mode", kPinningModes);
        reg.core_topology = opt_string(j, "core_topology");
        reg.bench_ref_commit = opt_string(j, "bench_ref_commit");
        reg.fastchess_version = opt_string(j, "fastchess_version");
        reg.nproc_total = opt_int(j, "nproc", 1);
        reg.slots = req_int(j, "slots", 1);

        auto conn = pool.acquire();
        const auto c = register_client(*conn, reg);
        if (c.banned) {
            send_json(res, 403, {{"error", "client_banned"}, {"client_id", c.client_id}});
            return;
        }
        send_json(res, 200, {{"client_id", c.client_id}});
    }));

    svr.Post(R"(/api/clients/(\d+)/heartbeat)",
             guarded([&](const httplib::Request& req, httplib::Response& res) {
        const auto j = parse_body(req);
        const auto cpu_factor = opt_double(j, "cpu_factor");
        if (cpu_factor && !(*cpu_factor > 0)) throw BadRequest("cpu_factor must be positive");

        auto conn = pool.acquire();
        const auto banned = heartbeat(*conn, path_id(req), cpu_factor);
        if (!banned) {
            send_error(res, 404, "unknown_client");
            return;
        }
        send_json(res, 200, {{"banned", *banned}});
    }));

    // --- teste ativo -------------------------------------------------------
    svr.Get("/api/active_test", guarded([&](const httplib::Request&, httplib::Response& res) {
        auto conn = pool.acquire();
        const auto t = active_test(*conn);
        if (!t) {
            res.status = 204;
            return;
        }
        send_json(res, 200,
                  {{"test_id", t->test_id},
                   {"kind", t->kind},
                   {"candidate_commit", t->candidate_commit},
                   {"baseline_commit", t->baseline_commit},
                   {"tc_base_ms", t->tc_base_ms},
                   {"tc_increment_ms", t->tc_increment_ms},
                   {"hash_mb", t->hash_mb},
                   {"threads", t->threads},
                   {"book_name", t->book_name},
                   {"candidate_tarball_url", tarball_url(cfg, t->candidate_commit)},
                   {"baseline_tarball_url", tarball_url(cfg, t->baseline_commit)},
                   {"adjudication", adjudication_json(t->adjudication)},
                   {"bench_reference_commit", cfg.bench_reference_commit.empty()
                                                  ? json(nullptr)
                                                  : json(cfg.bench_reference_commit)}});
    }));

    // --- jobs --------------------------------------------------------------
    svr.Post("/api/jobs/claim", guarded([&](const httplib::Request& req, httplib::Response& res) {
        const auto j = parse_body(req);
        const auto client_id = req_int64(j, "client_id");
        const auto slots_free = req_int(j, "slots_free", 0);

        auto conn = pool.acquire();
        const auto c = claim_pairs(*conn, cfg, client_id, slots_free);
        switch (c.status) {
            case ClaimStatus::UnknownClient: send_error(res, 404, "unknown_client"); return;
            case ClaimStatus::Banned: send_error(res, 403, "client_banned"); return;
            case ClaimStatus::NoActiveTest: res.status = 204; return;
            case ClaimStatus::Ok: break;
        }
        json pairs = json::array();
        for (const auto& p : c.pairs) {
            pairs.push_back({{"pair_id", p.pair_id},
                             {"position_id", p.position_id},
                             {"position_seq", p.position_seq},
                             {"fen", p.fen},
                             {"lease_expires_at", p.lease_expires_at}});
        }
        send_json(res, 200, {{"test_id", c.test_id}, {"pairs", pairs}});
    }));

    svr.Post(R"(/api/jobs/(\d+)/result)",
             guarded([&](const httplib::Request& req, httplib::Response& res) {
        const auto pair_id = path_id(req);
        const auto j = parse_body(req);

        PairReport report;
        report.client_id = req_int64(j, "client_id");
        const auto& games = field(j, "games");
        if (!games.is_array() || games.size() != 2) {
            throw BadRequest("games must be an array with exactly 2 games");
        }
        for (const auto& g : games) {
            auto game = parse_game(g);
            report.games[game.game_in_pair] = std::move(game);
        }
        if (games[0]["game_in_pair"] == games[1]["game_in_pair"]) {
            throw BadRequest("games must have game_in_pair 0 and 1");
        }

        auto conn = pool.acquire();
        const auto r = submit_result(*conn, pair_id, report);
        switch (r.status) {
            case SubmitStatus::Accepted:
                send_json(res, 200, {{"status", "accepted"}, {"valid_games", r.valid_games}});
                return;
            case SubmitStatus::PairNotFound: send_error(res, 404, "pair_not_found"); return;
            case SubmitStatus::UnknownClient: send_error(res, 404, "unknown_client"); return;
            case SubmitStatus::LeaseLost: send_error(res, 409, "conflict", "lease_lost"); return;
            case SubmitStatus::AlreadyCompleted:
                send_error(res, 409, "conflict", "already_completed");
                return;
            case SubmitStatus::TestFinished:
                send_error(res, 409, "conflict", "test_finished");
                return;
            case SubmitStatus::ClientBanned:
                send_error(res, 409, "conflict", "client_banned");
                return;
        }
    }));

    svr.Post(R"(/api/jobs/(\d+)/abandon)",
             guarded([&](const httplib::Request& req, httplib::Response& res) {
        const auto pair_id = path_id(req);
        const auto j = parse_body(req);
        const auto client_id = req_int64(j, "client_id");

        auto conn = pool.acquire();
        if (!abandon_pair(*conn, pair_id, client_id)) {
            send_error(res, 409, "conflict", "lease_lost");
            return;
        }
        send_json(res, 200, {{"status", "released"}});
    }));

    // --- leitura -----------------------------------------------------------
    svr.Get(R"(/api/tests/(\d+)/stats)",
            guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto conn = pool.acquire();
        const auto s = load_test_stats(*conn, path_id(req));
        if (!s) {
            send_error(res, 404, "test_not_found");
            return;
        }
        json body{
            {"test_id", s->test_id},
            {"name", s->name},
            {"kind", s->kind},
            {"status", s->status},
            {"result", s->result},
            {"priority", s->priority},
            {"candidate", {{"ref", s->candidate_ref}, {"commit", s->candidate_commit}}},
            {"baseline", {{"ref", s->baseline_ref}, {"commit", s->baseline_commit}}},
            {"tc_base_ms", s->tc_base_ms},
            {"tc_increment_ms", s->tc_increment_ms},
            {"hash_mb", s->hash_mb},
            {"threads", s->threads},
            {"book_name", s->book_name},
            {"created_at", s->created_at},
            {"started_at", nullable(s->started_at)},
            {"finished_at", nullable(s->finished_at)},
            {"adjudication", adjudication_json(s->adjudication)},
            {"pairs",
             {{"total", s->total_pairs},
              {"valid", s->penta.pairs()},
              {"pending", s->pairs_pending},
              {"leased", s->pairs_leased},
              {"completed", s->pairs_completed},
              {"discarded", s->pairs_discarded}}},
            {"penta",
             {{"ll", s->penta.counts[0]},
              {"ld", s->penta.counts[1]},
              {"dd_wl", s->penta.counts[2]},
              {"wd", s->penta.counts[3]},
              {"ww", s->penta.counts[4]}}},
            {"elo",
             {{"value", nullable(s->elo)},
              {"ci_low", nullable(s->elo_ci_low)},
              {"ci_high", nullable(s->elo_ci_high)}}},
            {"updated_at", nullable(s->stats_updated_at)},
        };
        if (s->kind == "sprt") {
            body["sprt"] = {{"elo0", nullable(s->sprt_elo0)},
                            {"elo1", nullable(s->sprt_elo1)},
                            {"alpha", nullable(s->sprt_alpha)},
                            {"beta", nullable(s->sprt_beta)},
                            {"llr", nullable(s->llr)},
                            {"llr_lower", nullable(s->llr_lower)},
                            {"llr_upper", nullable(s->llr_upper)}};
        }
        send_json(res, 200, body);
    }));
}

}  // namespace capi::server::http
