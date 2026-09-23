#pragma once

#include <pqxx/pqxx>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace capi::server::db {

// Pool de conexões libpqxx. Conexões não são thread-safe, e o cpp-httplib
// atende cada requisição numa thread própria: cada handler pega uma conexão
// exclusiva por RAII e a devolve ao sair. Conexões são criadas sob demanda
// até `size`; conexões quebradas são descartadas na devolução.
class ConnectionPool {
public:
    ConnectionPool(std::string conninfo, std::size_t size);

    class Lease {
    public:
        Lease(ConnectionPool& pool, std::unique_ptr<pqxx::connection> conn)
            : pool_(&pool), conn_(std::move(conn)) {}
        Lease(Lease&&) noexcept = default;
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        ~Lease();

        pqxx::connection& operator*() { return *conn_; }
        pqxx::connection* operator->() { return conn_.get(); }

    private:
        ConnectionPool* pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    // Bloqueia até haver conexão livre (ou criável).
    Lease acquire();

private:
    void release(std::unique_ptr<pqxx::connection> conn);

    std::string conninfo_;
    std::size_t size_;
    std::size_t created_ = 0;
    std::vector<std::unique_ptr<pqxx::connection>> idle_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace capi::server::db
