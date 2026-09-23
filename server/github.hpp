#pragma once

#include "server/config.hpp"

#include <stdexcept>
#include <string>

namespace capi::server {

struct GithubError : std::runtime_error {
    GithubError(const std::string& msg, bool not_found)
        : std::runtime_error(msg), not_found(not_found) {}
    bool not_found;  // ref inexistente (vs. API indisponível / erro de rede)
};

// Resolve branch, tag/release ou SHA (curto ou longo) para o SHA completo via
// GET {github_api}/repos/{repo}/commits/{ref} (plano §5.1 passo 2).
// O ref já deve ter passado por valid_ref(). Lança GithubError.
std::string resolve_ref(const ServerConfig& cfg, const std::string& ref);

// URL do tarball de um commit (§8.2).
std::string tarball_url(const ServerConfig& cfg, const std::string& sha);

bool is_full_sha(const std::string& s);

}  // namespace capi::server
