#include "server/http/admin_routes.hpp"

#include "server/admin.hpp"
#include "server/books.hpp"
#include "server/github.hpp"
#include "server/http/helpers.hpp"
#include "server/log.hpp"
#include "server/spec.hpp"

#include <filesystem>

namespace capi::server::http {

namespace fs = std::filesystem;

void register_admin_routes(httplib::Server& svr, db::ConnectionPool& pool, const ServerConfig& cfg,
                           SmokeWorker& smoke) {
    // Criação de teste (§5.1 passos 1–3). O smoke test e a geração de jobs
    // (passos 4–6) seguem assíncronos no SmokeWorker.
    svr.Post("/api/admin/tests", guarded([&](const httplib::Request& req, httplib::Response& res) {
        const auto body = parse_body(req);
        TestSpec spec;
        try {
            spec = parse_test_spec(body, cfg.sprt_presets);
        } catch (const SpecError& e) {
            send_error(res, 400, "invalid_spec", e.what());
            return;
        }

        const fs::path book = fs::path(cfg.books_dir) / spec.book_name;
        if (!fs::is_regular_file(book)) {
            send_error(res, 400, "invalid_spec", "book not found: " + spec.book_name);
            return;
        }
        const auto available = count_book_positions(book);
        if (static_cast<std::size_t>(spec.total_pairs) > available) {
            send_error(res, 400, "invalid_spec",
                       "total_pairs (" + std::to_string(spec.total_pairs) + ") exceeds the " +
                           std::to_string(available) + " positions in " + spec.book_name);
            return;
        }

        // Resolvidos uma única vez e congelados (§5.1 passo 2).
        std::string candidate, baseline;
        try {
            candidate = resolve_ref(cfg, spec.candidate_ref);
            baseline = resolve_ref(cfg, spec.baseline_ref);
        } catch (const GithubError& e) {
            if (e.not_found) {
                send_error(res, 400, "ref_not_found", e.what());
            } else {
                log_warn(std::string("github: ") + e.what());
                send_error(res, 502, "github_unavailable", e.what());
            }
            return;
        }
        if (candidate == baseline) {
            send_error(res, 400, "invalid_spec",
                       "candidate and baseline resolve to the same commit " + candidate);
            return;
        }

        auto conn = pool.acquire();
        const auto id = insert_test(*conn, spec, candidate, baseline);
        log_info("test " + std::to_string(id) + " created: " + spec.candidate_ref + " (" +
                 candidate + ") vs " + spec.baseline_ref + " (" + baseline + ")");
        smoke.notify();
        send_json(res, 201,
                  {{"test_id", id},
                   {"status", "validating"},
                   {"candidate", {{"ref", spec.candidate_ref}, {"commit", candidate}}},
                   {"baseline", {{"ref", spec.baseline_ref}, {"commit", baseline}}}});
    }));

    svr.Get(R"(/api/admin/tests/(\d+)/validation)",
            guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto conn = pool.acquire();
        const auto v = get_validation(*conn, path_id(req));
        if (!v) {
            send_error(res, 404, "test_not_found");
            return;
        }
        send_json(res, 200, {{"status", v->status}, {"validation_log", nullable(v->log)}});
    }));

    svr.Patch(R"(/api/admin/tests/(\d+))",
              guarded([&](const httplib::Request& req, httplib::Response& res) {
        const auto body = parse_body(req);
        const auto priority = req_int(body, "priority", INT32_MIN);

        auto conn = pool.acquire();
        const auto r = set_priority(*conn, path_id(req), priority);
        switch (r.status) {
            case PriorityStatus::NotFound: send_error(res, 404, "test_not_found"); return;
            case PriorityStatus::NotQueued:
                send_error(res, 409, "conflict", "test is " + r.test_status);
                return;
            case PriorityStatus::Updated:
                send_json(res, 200, {{"status", r.test_status}, {"priority", priority}});
                return;
        }
    }));

    svr.Post(R"(/api/admin/tests/(\d+)/stop)",
             guarded([&](const httplib::Request& req, httplib::Response& res) {
        const auto id = path_id(req);
        auto conn = pool.acquire();
        const auto r = stop_test(*conn, id);
        switch (r.status) {
            case StopStatus::NotFound: send_error(res, 404, "test_not_found"); return;
            case StopStatus::AlreadyTerminal:
                send_error(res, 409, "conflict", "test is already " + r.previous_status);
                return;
            case StopStatus::Stopped:
                log_info("test " + std::to_string(id) + " stopped manually (was " +
                         r.previous_status + ")");
                if (r.promoted) {
                    log_info("test " + std::to_string(*r.promoted) + " promoted to running");
                }
                send_json(res, 200, {{"status", "stopped"},
                                     {"previous_status", r.previous_status},
                                     {"promoted_test_id", nullable(r.promoted)}});
                return;
        }
    }));

    svr.Get("/api/tests", guarded([&](const httplib::Request& req, httplib::Response& res) {
        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
            } catch (const std::exception&) {
                throw BadRequest("invalid limit");
            }
            if (limit < 1 || limit > 1000) throw BadRequest("limit must be in [1, 1000]");
        }
        auto conn = pool.acquire();
        json out = json::array();
        for (const auto& t : list_tests(*conn, limit)) {
            out.push_back({{"test_id", t.id},
                           {"name", t.name},
                           {"kind", t.kind},
                           {"status", t.status},
                           {"result", t.result},
                           {"priority", t.priority},
                           {"candidate", {{"ref", t.candidate_ref}, {"commit", t.candidate_commit}}},
                           {"baseline", {{"ref", t.baseline_ref}, {"commit", t.baseline_commit}}},
                           {"total_pairs", t.total_pairs},
                           {"pairs_valid", t.pairs_valid},
                           {"llr", nullable(t.llr)},
                           {"llr_lower", nullable(t.llr_lower)},
                           {"llr_upper", nullable(t.llr_upper)},
                           {"elo", nullable(t.elo)},
                           {"created_at", t.created_at}});
        }
        send_json(res, 200, out);
    }));
}

}  // namespace capi::server::http
