#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace capi::build {

struct BuildOptions {
    std::filesystem::path work_dir;         // builds em {work_dir}/builds/{cache_key}/capizero
    std::string tarball_url;                // codeload do commit
    std::string cache_key;                  // ex: "<sha>" (server) ou "<sha>-<arch>" (client)
    std::vector<std::string> make_args;     // extras para `make build`
    const std::atomic<bool>* cancel = nullptr;
};

// Baixa o tarball e compila com `make build NAME=capizero` (plano §8.2), com
// cache por cache_key. Makefiles antigos que ignoram NAME são aceitos se
// gerarem um único executável capizero*. PEXT=false é adicionado quando a CPU
// não tem BMI2. Anexa o que fez a `log`; nullopt em falha.
std::optional<std::filesystem::path> build_engine(const BuildOptions& opts, std::string& log);

// `bench <depth>` com saída "Nodes: N" (N > 0) e handshake uci/isready.
bool check_engine(const std::filesystem::path& bin, int bench_depth, std::string& log,
                  const std::atomic<bool>* cancel = nullptr);

bool host_has_bmi2();

// Rótulo do alvo de build desta CPU (ex: "x86-64-bmi2", "armv8"). O Makefile
// do capizero compila com -march=native; o rótulo identifica o cache e é
// reportado ao server (clients.cpu_arch_target).
std::string host_arch_target();

}  // namespace capi::build
