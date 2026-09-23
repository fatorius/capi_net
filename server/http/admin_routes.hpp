#pragma once

#include "server/config.hpp"
#include "server/db/pool.hpp"
#include "server/smoke.hpp"

#include <httplib.h>

namespace capi::server::http {

// Criação e gestão de testes (plano §5.1, §5.8, §9) e listagem pública.
// v1: sem autenticação (rede interna confiável, §12).
void register_admin_routes(httplib::Server& svr, db::ConnectionPool& pool, const ServerConfig& cfg,
                           SmokeWorker& smoke);

}  // namespace capi::server::http
