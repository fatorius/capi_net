#include "server/db/pool.hpp"

namespace capi::server::db {

ConnectionPool::ConnectionPool(std::string conninfo, std::size_t size)
    : conninfo_(std::move(conninfo)), size_(size) {}

ConnectionPool::Lease::~Lease() {
    if (conn_) pool_->release(std::move(conn_));
}

ConnectionPool::Lease ConnectionPool::acquire() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return !idle_.empty() || created_ < size_; });

    if (!idle_.empty()) {
        auto conn = std::move(idle_.back());
        idle_.pop_back();
        return Lease(*this, std::move(conn));
    }

    // Reserva a vaga antes de conectar fora do lock.
    ++created_;
    lock.unlock();
    try {
        return Lease(*this, std::make_unique<pqxx::connection>(conninfo_));
    } catch (...) {
        lock.lock();
        --created_;
        cv_.notify_one();
        throw;
    }
}

void ConnectionPool::release(std::unique_ptr<pqxx::connection> conn) {
    std::lock_guard lock(mutex_);
    if (conn->is_open()) {
        idle_.push_back(std::move(conn));
    } else {
        --created_;
    }
    cv_.notify_one();
}

}  // namespace capi::server::db
