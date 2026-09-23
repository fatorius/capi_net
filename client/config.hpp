#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace capi::client {

// Arquivo "chave = valor" (# comenta), sobrescrito pelas flags de mesmo nome:
//
//   server    = http://<host-do-server>:8080
//   name      = meu-host-2slots      # padrão: <hostname>-<N>slots
//   slots     = 1                    # padrão: NPROC/4
//   cores     = 3                    # opcional: núcleos (um por slot)
//   work_dir  = ~/.cache/capi_net    # builds e arquivos temporários
//   fastchess = /caminho/fastchess   # opcional: padrão é compilar kFastchessTag
//   make_args = COMP=clang           # extras para `make build`
struct ClientConfig {
    std::string server = "http://localhost:8080";
    std::string name;
    std::optional<int> slots;
    std::optional<std::vector<int>> cores;
    std::string work_dir;
    std::string fastchess;  // vazio = versão fixa compilada pelo client
    std::vector<std::string> make_args;
    std::chrono::seconds poll_interval{10};

    int max_pairs = 0;   // --max-pairs: sai após N pares (0 = sem limite; útil em testes)
    bool check = false;  // --check: só mostra topologia/admissão e sai
};

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Lê --config ARQ (ou ~/.config/capi_net/client.conf, se existir) e aplica as
// flags da linha de comando por cima. Lança ConfigError.
ClientConfig load_client_config(int argc, char** argv);

extern const char* kClientUsage;

}  // namespace capi::client
