#pragma once

#include "server/config.hpp"
#include "server/db/pool.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace capi::server {

// Processa, um por vez, os testes em 'validating' (§5.1 passos 4–6): smoke
// test dos dois commits; se passar, amostra as posições do book, cria os
// job_pairs e move o teste para 'queued' (promovendo-o se a fila estiver
// livre); se falhar, 'invalid' com validation_log. Retoma testes que ficaram
// em 'validating' após um restart.
class SmokeWorker {
public:
    SmokeWorker(db::ConnectionPool& pool, const ServerConfig& cfg);
    ~SmokeWorker();  // cancela o smoke test em curso (o teste fica em 'validating')

    void notify();  // há teste novo para validar

private:
    void run();
    bool process_next();

    db::ConnectionPool& pool_;
    const ServerConfig& cfg_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ = true;
    std::thread thread_;  // último membro: inicia após os demais
};

}  // namespace capi::server
