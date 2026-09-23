#pragma once

#include "server/spec.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace capi::server {

// Configuração do server, lida de variáveis de ambiente (ver load_config_from_env).
struct ServerConfig {
    std::string db_url = "dbname=capi_net";   // CAPI_DB_URL (conninfo libpq)
    std::string http_host = "0.0.0.0";        // CAPI_HTTP_HOST
    int http_port = 8080;                     // CAPI_HTTP_PORT
    int db_pool_size = 8;                     // CAPI_DB_POOL_SIZE (= threads HTTP)

    // Lease = max(lease_ttl_min, 6 × duração estimada do par) (plano §5.3).
    std::chrono::seconds lease_ttl_min{600};  // CAPI_LEASE_TTL_MIN_S
    int max_attempts = 5;                     // CAPI_MAX_ATTEMPTS (§5.4)
    std::chrono::seconds maintenance_interval{30};  // CAPI_MAINTENANCE_INTERVAL_S

    // Commit fixo usado na calibração de cpu_factor (§3.2); repassado aos clients.
    std::string bench_reference_commit;       // CAPI_BENCH_REFERENCE_COMMIT

    // Repositório das engines (v1: fixo) e endpoints do GitHub (sobrescrevíveis
    // para testes).
    std::string github_repo = "fatorius/capizero";              // CAPI_GITHUB_REPO
    std::string github_api = "https://api.github.com";          // CAPI_GITHUB_API
    std::string github_codeload = "https://codeload.github.com"; // CAPI_GITHUB_CODELOAD
    std::string github_token;  // CAPI_GITHUB_TOKEN (opcional; sobe o rate limit da API)

    std::string books_dir = "books";  // CAPI_BOOKS_DIR
    std::string work_dir = "work";    // CAPI_WORK_DIR (downloads e builds do smoke test)
    std::string web_dir = "web";      // CAPI_WEB_DIR (páginas e assets da web UI)

    // Smoke test (§5.1 passo 4).
    int smoke_bench_depth = 6;                 // CAPI_SMOKE_BENCH_DEPTH
    std::vector<std::string> build_make_args;  // CAPI_BUILD_MAKE_ARGS (separados por espaço)

    SprtPresets sprt_presets;
};

// Lança std::runtime_error se algum valor numérico for inválido.
ServerConfig load_config_from_env();

}  // namespace capi::server
