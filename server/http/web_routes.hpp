#pragma once

#include "server/config.hpp"
#include "server/db/pool.hpp"

#include <httplib.h>

namespace capi::server::http {

// Web UI (plano §11): páginas estáticas em {web_dir} que consomem a API, e os
// endpoints de leitura que elas usam (histórico, clients, PGN, books, config).
void register_web_routes(httplib::Server& svr, db::ConnectionPool& pool, const ServerConfig& cfg);

}  // namespace capi::server::http
