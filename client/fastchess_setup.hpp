#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

namespace capi::client {

// Versão do fastchess usada por todos os clients (§3.6: o harness precisa ser
// o mesmo em todo lugar; um bug nele enviesa o LLR em silêncio). Trocar a
// versão aqui faz cada client compilar a nova no próximo start.
inline constexpr const char* kFastchessTag = "v1.8.2-alpha";

struct FastchessInfo {
    std::filesystem::path path;
    std::string version;  // primeira linha de `fastchess -version`
};

// Com `configured` vazio, garante o fastchess kFastchessTag compilado em
// {work_dir}/tools/fastchess-{tag}/fastchess (baixa o tarball do GitHub e roda
// make, só na primeira vez). Com `configured`, usa esse binário (--fastchess).
// Em ambos os casos confere `-version`. Anexa o que fez a `log`.
std::optional<FastchessInfo> ensure_fastchess(const std::string& configured,
                                              const std::filesystem::path& work_dir,
                                              std::string& log,
                                              const std::atomic<bool>* cancel = nullptr);

// URL do tarball da tag no GitHub.
std::string fastchess_tarball_url(const std::string& tag);

}  // namespace capi::client
