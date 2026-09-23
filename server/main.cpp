#include "server/config.hpp"
#include "server/db/pool.hpp"
#include "server/http/admin_routes.hpp"
#include "server/http/routes.hpp"
#include "server/http/web_routes.hpp"
#include "server/log.hpp"
#include "server/queue.hpp"
#include "server/smoke.hpp"
#include "server/test_lifecycle.hpp"

#include <httplib.h>

#include <pthread.h>
#include <csignal>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace capi::server;

namespace {

constexpr int kRequiredSchemaVersion = 3;

// Tarefas periódicas (§5.1 passo 6, §5.4): recuperação de leases expirados e
// promoção do próximo teste da fila.
void maintenance_tick(db::ConnectionPool& pool, const ServerConfig& cfg) {
    auto conn = pool.acquire();

    const auto rec = recover_expired_leases(*conn, cfg.max_attempts);
    if (rec.requeued || rec.discarded) {
        log_info("expired leases: " + std::to_string(rec.requeued) + " requeued, " +
                 std::to_string(rec.discarded) + " discarded");
    }

    pqxx::work tx{*conn};
    if (auto id = promote_next_test(tx)) {
        log_info("test " + std::to_string(*id) + " promoted to running");
    }
    tx.commit();
}

class Maintenance {
public:
    Maintenance(db::ConnectionPool& pool, const ServerConfig& cfg)
        : thread_([this, &pool, &cfg] { run(pool, cfg); }) {}

    ~Maintenance() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

private:
    void run(db::ConnectionPool& pool, const ServerConfig& cfg) {
        std::unique_lock lock(mutex_);
        while (!stop_) {
            lock.unlock();
            try {
                maintenance_tick(pool, cfg);
            } catch (const std::exception& e) {
                log_error(std::string("maintenance: ") + e.what());
            }
            lock.lock();
            cv_.wait_for(lock, cfg.maintenance_interval, [&] { return stop_; });
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;  // último membro: inicia após os demais
};

}  // namespace

int main() {
    ServerConfig cfg;
    try {
        cfg = load_config_from_env();
    } catch (const std::exception& e) {
        log_error(std::string("config: ") + e.what());
        return 2;
    }

    // Bloqueia SIGINT/SIGTERM em todas as threads; uma thread dedicada os
    // recebe via sigwait e para o servidor de forma limpa.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    db::ConnectionPool pool(cfg.db_url, static_cast<std::size_t>(cfg.db_pool_size));
    try {
        auto conn = pool.acquire();
        pqxx::nontransaction tx{*conn};
        const auto version = tx.query_value<int>("SELECT max(version) FROM schema_migrations");
        if (version < kRequiredSchemaVersion) {
            log_error("database schema is at version " + std::to_string(version) + ", " +
                      std::to_string(kRequiredSchemaVersion) +
                      " required: run scripts/migrate.sh");
            return 1;
        }
        log_info("connected to database (schema version " + std::to_string(version) + ")");
    } catch (const std::exception& e) {
        log_error(std::string("database: ") + e.what());
        return 1;
    }

    // Destruído depois do servidor HTTP (declarado antes); cancela um smoke
    // test em curso no shutdown.
    SmokeWorker smoke(pool, cfg);

    httplib::Server svr;
    capi::server::http::register_routes(svr, pool, cfg);
    capi::server::http::register_admin_routes(svr, pool, cfg, smoke);
    capi::server::http::register_web_routes(svr, pool, cfg);

    std::atomic<bool> signalled{false};
    std::thread signal_thread([&] {
        int sig = 0;
        sigwait(&signals, &sig);
        signalled = true;
        log_info("signal " + std::to_string(sig) + " received, shutting down");
        svr.stop();
    });

    int rc = 0;
    {
        Maintenance maintenance(pool, cfg);
        log_info("listening on " + cfg.http_host + ":" + std::to_string(cfg.http_port));
        if (!svr.listen(cfg.http_host, cfg.http_port)) {
            if (!signalled) {
                log_error("failed to listen on " + cfg.http_host + ":" +
                          std::to_string(cfg.http_port));
                rc = 1;
            }
        }
    }

    if (!signalled) pthread_kill(signal_thread.native_handle(), SIGTERM);
    signal_thread.join();
    return rc;
}
