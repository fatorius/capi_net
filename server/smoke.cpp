#include "server/smoke.hpp"

#include "common/engine_build.hpp"
#include "server/books.hpp"
#include "server/github.hpp"
#include "server/log.hpp"
#include "server/test_lifecycle.hpp"

#include <pqxx/pqxx>

#include <chrono>

namespace capi::server {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// SmokeWorker
// ---------------------------------------------------------------------------
SmokeWorker::SmokeWorker(db::ConnectionPool& pool, const ServerConfig& cfg)
    : pool_(pool), cfg_(cfg), thread_([this] { run(); }) {}

SmokeWorker::~SmokeWorker() {
    stop_ = true;
    cv_.notify_all();
    thread_.join();
}

void SmokeWorker::notify() {
    {
        std::lock_guard lock(mutex_);
        pending_ = true;
    }
    cv_.notify_all();
}

void SmokeWorker::run() {
    while (!stop_) {
        {
            std::unique_lock lock(mutex_);
            // Sem notificação, reexamina a cada 30 s (ex: teste inserido por fora).
            cv_.wait_for(lock, 30s, [&] { return pending_ || stop_.load(); });
            pending_ = false;
        }
        try {
            while (!stop_ && process_next()) {
            }
        } catch (const std::exception& e) {
            log_error(std::string("smoke worker: ") + e.what());
        }
    }
}

bool SmokeWorker::process_next() {
    std::int64_t test_id;
    std::string candidate, baseline, book_name;
    int total_pairs;
    {
        auto conn = pool_.acquire();
        pqxx::read_transaction tx{*conn};
        const auto r = tx.exec(R"(
            SELECT id, candidate_commit, baseline_commit, book_name, total_pairs
            FROM tests WHERE status = 'validating' ORDER BY id LIMIT 1)");
        if (r.empty()) return false;
        test_id = r[0][0].as<std::int64_t>();
        candidate = r[0][1].as<std::string>();
        baseline = r[0][2].as<std::string>();
        book_name = r[0][3].as<std::string>();
        total_pairs = r[0][4].as<int>();
    }
    log_info("smoke test for test " + std::to_string(test_id) + " started");

    // Sem conexão presa durante o build (pode levar minutos).
    std::string vlog;
    bool ok = true;
    std::vector<std::string> positions;
    try {
        for (const auto& [role, sha] : {std::pair{"candidate", candidate}, {"baseline", baseline}}) {
            vlog += std::string("== ") + role + " " + sha + " ==\n";
            build::BuildOptions b;
            b.work_dir = cfg_.work_dir;
            b.tarball_url = tarball_url(cfg_, sha);
            b.cache_key = sha;
            b.make_args = cfg_.build_make_args;
            b.cancel = &stop_;
            const auto bin = build::build_engine(b, vlog);
            ok = bin && build::check_engine(*bin, cfg_.smoke_bench_depth, vlog, &stop_);
            if (!ok) break;
        }
        if (ok) {
            const fs::path book = fs::path(cfg_.books_dir) / book_name;
            positions = sample_book(book, static_cast<std::size_t>(total_pairs),
                                    static_cast<std::uint64_t>(test_id));
            if (positions.size() < static_cast<std::size_t>(total_pairs)) {
                vlog += "book " + book_name + " has only " + std::to_string(positions.size()) +
                        " positions, " + std::to_string(total_pairs) + " required\n";
                ok = false;
            } else {
                vlog += "sampled " + std::to_string(positions.size()) + " positions from " +
                        book_name + " (seed " + std::to_string(test_id) + ")\n";
            }
        }
    } catch (const std::exception& e) {
        vlog += std::string("internal error: ") + e.what() + "\n";
        ok = false;
    }

    if (stop_) {
        log_info("smoke test for test " + std::to_string(test_id) +
                 " interrupted by shutdown; will resume on restart");
        return false;
    }

    auto conn = pool_.acquire();
    pqxx::work tx{*conn};
    const auto status = tx.query_value<std::string>(
        "SELECT status::text FROM tests WHERE id = $1 FOR UPDATE", pqxx::params{test_id});
    if (status != "validating") {
        log_info("test " + std::to_string(test_id) + " left 'validating' during smoke test (now " +
                 status + "); result discarded");
        return true;
    }

    if (!ok) {
        vlog += "SMOKE TEST FAILED\n";
        tx.exec("UPDATE tests SET status = 'invalid', validation_log = $2 WHERE id = $1",
                pqxx::params{test_id, vlog});
        tx.commit();
        log_warn("test " + std::to_string(test_id) + " is invalid (smoke test failed)");
        return true;
    }

    vlog += "SMOKE TEST PASSED\n";
    {
        auto stream = pqxx::stream_to::table(tx, {"opening_positions"}, {"test_id", "seq", "fen"});
        for (std::size_t i = 0; i < positions.size(); ++i) {
            stream.write_values(test_id, static_cast<int>(i + 1), positions[i]);
        }
        stream.complete();
    }
    // dispatch_order aleatório: alocação de posições independe de qual client
    // conecta primeiro (§5.2).
    tx.exec(R"(
        INSERT INTO job_pairs (test_id, position_id, dispatch_order)
        SELECT test_id, id, random() FROM opening_positions WHERE test_id = $1)",
            pqxx::params{test_id});
    tx.exec("UPDATE tests SET status = 'queued', validation_log = $2 WHERE id = $1",
            pqxx::params{test_id, vlog});
    refresh_test(tx, test_id);  // cria test_stats (fronteiras do SPRT já visíveis)
    const auto promoted = promote_next_test(tx);
    tx.commit();

    log_info("test " + std::to_string(test_id) + " queued with " +
             std::to_string(positions.size()) + " pairs");
    if (promoted) log_info("test " + std::to_string(*promoted) + " promoted to running");
    return true;
}

}  // namespace capi::server
