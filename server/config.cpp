#include "server/config.hpp"

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>

namespace capi::server {

namespace {

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

int env_int(const char* name, int fallback, int min_value) {
    const char* v = env(name);
    if (!v) return fallback;
    std::size_t pos = 0;
    int parsed = 0;
    try {
        parsed = std::stoi(v, &pos);
    } catch (const std::exception&) {
        pos = 0;
    }
    if (pos == 0 || v[pos] != '\0' || parsed < min_value) {
        throw std::runtime_error(std::string(name) + ": invalid value '" + v + "'");
    }
    return parsed;
}

}  // namespace

ServerConfig load_config_from_env() {
    ServerConfig c;
    if (const char* v = env("CAPI_DB_URL")) c.db_url = v;
    if (const char* v = env("CAPI_HTTP_HOST")) c.http_host = v;
    if (const char* v = env("CAPI_BENCH_REFERENCE_COMMIT")) c.bench_reference_commit = v;
    if (const char* v = env("CAPI_GITHUB_REPO")) c.github_repo = v;
    if (const char* v = env("CAPI_GITHUB_API")) c.github_api = v;
    if (const char* v = env("CAPI_GITHUB_CODELOAD")) c.github_codeload = v;
    if (const char* v = env("CAPI_GITHUB_TOKEN")) c.github_token = v;
    if (const char* v = env("CAPI_BOOKS_DIR")) c.books_dir = v;
    if (const char* v = env("CAPI_WORK_DIR")) c.work_dir = v;
    if (const char* v = env("CAPI_WEB_DIR")) c.web_dir = v;
    if (const char* v = env("CAPI_BUILD_MAKE_ARGS")) {
        std::istringstream ss(v);
        for (std::string arg; ss >> arg;) c.build_make_args.push_back(arg);
    }
    c.smoke_bench_depth = env_int("CAPI_SMOKE_BENCH_DEPTH", c.smoke_bench_depth, 1);
    c.http_port = env_int("CAPI_HTTP_PORT", c.http_port, 1);
    c.db_pool_size = env_int("CAPI_DB_POOL_SIZE", c.db_pool_size, 1);
    c.lease_ttl_min = std::chrono::seconds(
        env_int("CAPI_LEASE_TTL_MIN_S", static_cast<int>(c.lease_ttl_min.count()), 1));
    c.max_attempts = env_int("CAPI_MAX_ATTEMPTS", c.max_attempts, 1);
    c.maintenance_interval = std::chrono::seconds(env_int(
        "CAPI_MAINTENANCE_INTERVAL_S", static_cast<int>(c.maintenance_interval.count()), 1));
    return c;
}

}  // namespace capi::server
