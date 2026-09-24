#include "server/http/web_routes.hpp"

#include "server/books.hpp"
#include "server/gzip.hpp"
#include "server/http/helpers.hpp"
#include "server/reports.hpp"
#include "server/spec.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace capi::server::http {

namespace fs = std::filesystem;

namespace {

void serve_page(httplib::Response& res, const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        send_error(res, 500, "web_ui_missing", "arquivo não encontrado: " + file.string());
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    res.set_header("Cache-Control", "no-cache");
    res.set_content(ss.str(), "text/html; charset=utf-8");
}

std::int64_t path_id_at(const httplib::Request& req, std::size_t i) {
    try {
        return std::stoll(req.matches[i].str());
    } catch (const std::exception&) {
        throw BadRequest("invalid id in path");
    }
}

}  // namespace

void register_web_routes(httplib::Server& svr, db::ConnectionPool& pool, const ServerConfig& cfg) {
    const fs::path web = cfg.web_dir;

    // --- páginas ------------------------------------------------------------
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/tests");
    });
    svr.Get("/tests", [web](const httplib::Request&, httplib::Response& res) {
        serve_page(res, web / "tests.html");
    });
    svr.Get("/clients", [web](const httplib::Request&, httplib::Response& res) {
        serve_page(res, web / "clients.html");
    });
    svr.Get("/tests/new", [web](const httplib::Request&, httplib::Response& res) {
        serve_page(res, web / "new.html");
    });
    svr.Get(R"(/tests/(\d+))", [web](const httplib::Request&, httplib::Response& res) {
        serve_page(res, web / "test.html");
    });
    if (!svr.set_mount_point("/static", (web / "static").string())) {
        log_warn("web UI: diretório " + (web / "static").string() + " não encontrado");
    }

    // --- dados das páginas -------------------------------------------------
    svr.Get("/api/config", guarded([&cfg](const httplib::Request&, httplib::Response& res) {
        auto preset = [](const SprtPresetBounds& b) {
            return json{{"elo0", b.elo0}, {"elo1", b.elo1}, {"alpha", b.alpha}, {"beta", b.beta}};
        };
        send_json(res, 200,
                  {{"github_repo", cfg.github_repo},
                   {"presets",
                    {{"gainer", preset(cfg.sprt_presets.gainer)},
                     {"nonreg", preset(cfg.sprt_presets.nonreg)}}}});
    }));

    svr.Get("/api/books", guarded([&cfg](const httplib::Request&, httplib::Response& res) {
        json out = json::array();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(cfg.books_dir, ec)) {
            const auto name = e.path().filename().string();
            if (!e.is_regular_file() || e.path().extension() != ".epd" || !valid_book_name(name)) {
                continue;
            }
            out.push_back({{"name", name},
                           {"size_bytes", e.file_size()},
                           {"positions", count_book_positions(e.path())}});
        }
        send_json(res, 200, out);
    }));

    svr.Get(R"(/api/tests/(\d+)/history)",
            guarded([&pool](const httplib::Request& req, httplib::Response& res) {
        const auto id = path_id(req);
        auto conn = pool.acquire();
        if (!test_exists(*conn, id)) {
            send_error(res, 404, "test_not_found");
            return;
        }
        json out = json::array();
        for (const auto& p : stats_history(*conn, id, 1000)) {
            out.push_back({{"pairs", p.pairs},
                           {"llr", nullable(p.llr)},
                           {"elo", nullable(p.elo)},
                           {"at", p.at}});
        }
        send_json(res, 200, out);
    }));

    svr.Get(R"(/api/tests/(\d+)/clients)",
            guarded([&pool](const httplib::Request& req, httplib::Response& res) {
        const auto id = path_id(req);
        auto conn = pool.acquire();
        if (!test_exists(*conn, id)) {
            send_error(res, 404, "test_not_found");
            return;
        }
        json out = json::array();
        for (const auto& c : test_clients(*conn, id)) {
            out.push_back({{"client_id", c.client_id},
                           {"name", c.name},
                           {"status", c.status},
                           {"pinning_mode", c.pinning_mode},
                           {"cpu_model", nullable(c.cpu_model)},
                           {"core_topology", nullable(c.core_topology)},
                           {"arch_target", nullable(c.arch_target)},
                           {"fastchess_version", nullable(c.fastchess_version)},
                           {"slots", c.slots},
                           {"cpu_factor", nullable(c.cpu_factor)},
                           {"pairs_completed", c.pairs_completed},
                           {"games_invalid", c.games_invalid},
                           {"pairs_leased", c.pairs_leased},
                           {"last_seen", c.last_seen},
                           {"online", c.online},
                           {"nps", nullable(c.nps)}});
        }
        send_json(res, 200, out);
    }));

    svr.Get("/api/clients", guarded([&pool](const httplib::Request&, httplib::Response& res) {
        auto conn = pool.acquire();
        json out = json::array();
        for (const auto& c : all_clients(*conn, 200)) {
            out.push_back({{"client_id", c.client_id},
                           {"name", c.name},
                           {"status", c.status},
                           {"pinning_mode", c.pinning_mode},
                           {"cpu_model", nullable(c.cpu_model)},
                           {"core_topology", nullable(c.core_topology)},
                           {"arch_target", nullable(c.arch_target)},
                           {"fastchess_version", nullable(c.fastchess_version)},
                           {"os", nullable(c.os)},
                           {"slots", c.slots},
                           {"last_seen", c.last_seen},
                           {"online", c.online},
                           {"pairs_leased", c.pairs_leased},
                           {"games_total", c.games_total},
                           {"nps", nullable(c.nps)},
                           {"nps_games", c.nps_games},
                           {"last_game_at", nullable(c.last_game_at)}});
        }
        send_json(res, 200, out);
    }));

    svr.Get(R"(/api/tests/(\d+)/speed)",
            guarded([&pool](const httplib::Request& req, httplib::Response& res) {
        const auto id = path_id(req);
        auto conn = pool.acquire();
        if (!test_exists(*conn, id)) {
            send_error(res, 404, "test_not_found");
            return;
        }
        const auto s = test_speed(*conn, id);
        json body{{"candidate_nps", nullable(s.candidate_nps)},
                  {"baseline_nps", nullable(s.baseline_nps)},
                  {"games", s.games},
                  {"ratio", nullptr}};
        if (s.candidate_nps && s.baseline_nps && *s.baseline_nps > 0) {
            body["ratio"] = *s.candidate_nps / *s.baseline_nps;
        }
        send_json(res, 200, body);
    }));

    svr.Get(R"(/api/tests/(\d+)/games/(\d+)/pgn)",
            guarded([&pool](const httplib::Request& req, httplib::Response& res) {
        auto conn = pool.acquire();
        const auto pgn = game_pgn(*conn, path_id_at(req, 1), path_id_at(req, 2));
        if (!pgn) {
            send_error(res, 404, "game_not_found");
            return;
        }
        res.set_content(*pgn, "application/x-chess-pgn; charset=utf-8");
    }));

    // PGN agregado do teste (§11), descomprimido em fluxo, em lotes.
    svr.Get(R"(/api/tests/(\d+)/pgn)",
            guarded([&pool](const httplib::Request& req, httplib::Response& res) {
        const auto id = path_id(req);
        {
            auto conn = pool.acquire();
            if (!test_exists(*conn, id)) {
                send_error(res, 404, "test_not_found");
                return;
            }
        }
        res.set_header("Content-Disposition",
                       "attachment; filename=\"capi_net-test-" + std::to_string(id) + ".pgn\"");
        auto cursor = std::make_shared<std::int64_t>(0);
        res.set_chunked_content_provider(
            "application/x-chess-pgn; charset=utf-8",
            [&pool, id, cursor](std::size_t, httplib::DataSink& sink) {
                try {
                    auto conn = pool.acquire();
                    const auto batch = pgn_batch(*conn, id, *cursor, 200);
                    if (batch.empty()) {
                        sink.done();
                        return true;
                    }
                    for (const auto& g : batch) {
                        const auto text = gzip_decompress(g.pgn_gz) + "\n";
                        if (!sink.write(text.data(), text.size())) return false;
                        *cursor = g.game_id;
                    }
                    return true;
                } catch (const std::exception& e) {
                    log_error("pgn stream of test " + std::to_string(id) + ": " + e.what());
                    return false;
                }
            });
    }));
}

}  // namespace capi::server::http
