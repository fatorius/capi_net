#pragma once

#include "client/api.hpp"
#include "client/config.hpp"
#include "client/topology.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace capi::client {

// Engines compilados e condições de jogo do teste ativo.
struct EngineSet {
    std::int64_t test_id = 0;
    std::filesystem::path candidate, baseline;
    int tc_base_ms = 0, tc_increment_ms = 0, hash_mb = 16, threads = 1;
    Adjudication adjudication;
};

struct SlotInfo {
    int index = 0;
    std::optional<int> core;  // núcleo fixado (vazio se homogeneous_unpinned)
};

// TC efetivo = TC do teste × cpu_factor (§3.2), em ms.
struct EffectiveTc {
    int base_ms, increment_ms;
};
EffectiveTc effective_tc(const EngineSet& e, double cpu_factor);

// "10+0.1" para o fastchess, a partir de ms.
std::string fastchess_tc(const EffectiveTc& tc);

// Linha de comando do fastchess para um par (§8.4): duas partidas da mesma
// FEN com cores trocadas, em sequência, sem -concurrency. A adjudicação do
// teste vira -draw/-resign.
std::vector<std::string> fastchess_args(const std::string& fastchess, const EngineSet& e,
                                        const EffectiveTc& tc,
                                        const std::filesystem::path& epd,
                                        const std::filesystem::path& pgn,
                                        const std::string& event);

// -draw movenumber=.. movecount=.. score=..  -resign movecount=.. score=.. twosided=..
std::vector<std::string> adjudication_args(const Adjudication& a);

// Corpo de POST /api/jobs/{id}/result a partir do PGN do fastchess. Partida
// ausente ou ilegível vira um registro com termination 'crash' e a nota no
// lugar do PGN: o server a invalida e ela fica auditável (§8.5, não silenciar).
nlohmann::json make_result_body(std::int64_t client_id, const std::string& pgn,
                                const std::string& failure_note, const SlotInfo& slot,
                                const EffectiveTc& tc, double cpu_factor);

// Loop do client (§8.1 passo 5): a thread principal faz heartbeat, acompanha o
// teste ativo e prepara os engines; cada slot pede um par, roda o fastchess
// fixado no seu núcleo e envia o resultado.
class Runner {
public:
    Runner(const ClientConfig& cfg, Api& api, std::int64_t client_id,
           std::vector<SlotInfo> slots, std::string arch_target);

    // Bloqueia até `stop` virar true (sinal, --max-pairs ou banimento).
    // Retorna true se o client foi banido.
    bool run(std::atomic<bool>& stop);

    // Acorda esperas internas (chamar após mudar `stop`).
    void wake();

private:
    void slot_loop(SlotInfo slot);
    void poll_server();
    bool prepare_engines(const ActiveTest& t);
    std::optional<nlohmann::json> play_pair(const SlotInfo& slot, const EngineSet& e,
                                            const ClaimedPair& pair);
    void submit_with_retry(std::int64_t pair_id, const nlohmann::json& body);
    void sleep_for(std::chrono::milliseconds d);
    std::shared_ptr<const EngineSet> engines();
    void request_stop();

    const ClientConfig& cfg_;
    Api& api_;
    std::int64_t client_id_;
    std::vector<SlotInfo> slots_;
    std::string arch_target_;
    double cpu_factor_ = 1.0;  // calibração entra no passo 7 do §13

    std::atomic<bool>* stop_ = nullptr;
    std::atomic<bool> banned_{false};
    std::atomic<int> pairs_done_{0};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<const EngineSet> engines_;
    std::optional<std::int64_t> failed_test_;  // build falhou: não tentar de novo
};

}  // namespace capi::client
