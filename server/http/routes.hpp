#pragma once

#include "server/config.hpp"
#include "server/db/pool.hpp"

#include <httplib.h>

namespace capi::server::http {

// Registra as rotas da API de clients e de leitura (plano §9).
void register_routes(httplib::Server& svr, db::ConnectionPool& pool, const ServerConfig& cfg);

}  // namespace capi::server::http
