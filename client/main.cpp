// capi_net_client — executa pares de partidas para o server (plano §8).

#include "client/api.hpp"
#include "client/config.hpp"
#include "client/fastchess_setup.hpp"
#include "client/runner.hpp"
#include "client/topology.hpp"
#include "common/engine_build.hpp"
#include "common/log.hpp"
#include "common/process.hpp"

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

using namespace capi;
using namespace capi::client;
using namespace std::chrono_literals;

namespace {

std::string hostname() {
    char buf[256] = {};
    gethostname(buf, sizeof(buf) - 1);
    return buf;
}

std::string join(const std::vector<int>& v) {
    std::string s;
    for (int x : v) s += (s.empty() ? "" : ",") + std::to_string(x);
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    ClientConfig cfg;
    try {
        cfg = load_client_config(argc, argv);
    } catch (const ConfigError& e) {
        std::cerr << e.what() << (std::string(e.what()) == kClientUsage ? "" : "\n");
        return std::string(e.what()) == kClientUsage ? 0 : 2;
    }

    // §8.1 passos 2–3: topologia, admissão e teste de afinidade.
    const Topology topo = detect_topology();
    const int slots = cfg.slots.value_or(default_slots(topo));
    const Admission adm = decide_admission(topo, slots, cfg.cores);
    const std::string arch = build::host_arch_target();

    std::cout << "host:      " << hostname() << "\n"
              << "os:        " << topo.os << "\n"
              << "cpu:       " << topo.cpu_model << " (" << topo.nproc << " lógicos, "
              << topo.description << ")\n"
              << "arch:      " << arch << "\n"
              << "afinidade: " << (topo.pinning_available ? "disponível" : "indisponível") << "\n"
              << "slots:     " << slots << "\n";
    if (!adm.ok) {
        std::cerr << "\nclient recusado: " << adm.error << "\n";
        return 3;
    }
    if (adm.pinning_mode == "pinned") {
        std::string err;
        if (!verify_pinning(adm.cores, err)) {
            std::cerr << "\nclient recusado: " << err << "\n";
            return 3;
        }
    }
    std::cout << "admissão:  " << adm.pinning_mode
              << (adm.cores.empty() ? "" : " nos núcleos " + join(adm.cores)) << std::endl;
    if (cfg.check) return 0;

    // Sinais tratados numa thread dedicada: o primeiro SIGINT/SIGTERM
    // interrompe os pares em curso e devolve os leases.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);  // usado só para acordar a thread no fim
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    std::atomic<bool> stop{false};
    std::atomic<bool> signalled{false};
    Runner* runner_ptr = nullptr;
    std::mutex runner_mutex;
    std::thread signal_thread([&] {
        int sig = 0;
        sigwait(&signals, &sig);
        if (sig == SIGUSR1) return;  // encerramento normal
        signalled = true;
        log_info("sinal " + std::to_string(sig) + " recebido; devolvendo pares e encerrando");
        stop = true;
        std::lock_guard lock(runner_mutex);
        if (runner_ptr) runner_ptr->wake();
    });

    auto finish = [&](int rc) {
        // O runner também para sozinho (--max-pairs, banimento): a thread de
        // sinais continua em sigwait e precisa ser acordada.
        if (!signalled) pthread_kill(signal_thread.native_handle(), SIGUSR1);
        signal_thread.join();
        return rc;
    };

    // Harness: versão fixa, compilada na primeira execução (§3.6).
    if (cfg.fastchess.empty()) {
        log_info(std::string("preparando fastchess ") + kFastchessTag +
                 " (a primeira vez compila; pode levar alguns minutos)");
    }
    std::string fc_log;
    const auto fc = ensure_fastchess(cfg.fastchess, cfg.work_dir, fc_log, &stop);
    if (!fc) {
        if (stop) return finish(0);
        log_error("fastchess indisponível:\n" + fc_log);
        return finish(2);
    }
    cfg.fastchess = fc->path.string();
    log_info("usando " + fc->version + " (" + cfg.fastchess + ")");

    // §8.1 passo 4: registro (com retry enquanto o server estiver fora).
    Api api(cfg.server);
    const std::string name =
        cfg.name.empty() ? hostname() + "-" + std::to_string(slots) + "slots" : cfg.name;
    const nlohmann::json info{{"name", name},
                              {"hostname", hostname()},
                              {"os", topo.os},
                              {"cpu_model", topo.cpu_model},
                              {"arch_target", arch},
                              {"pinning_mode", adm.pinning_mode},
                              {"core_topology", adm.core_topology},
                              {"nproc", topo.nproc},
                              {"slots", slots},
                              {"fastchess_version", fc->version}};
    std::int64_t client_id = 0;
    auto backoff = 2s;
    while (!stop) {
        try {
            client_id = api.register_client(info);
            break;
        } catch (const TransientError& e) {
            log_warn(std::string("registro falhou (") + e.what() + "); nova tentativa em " +
                     std::to_string(backoff.count()) + "s");
            for (auto waited = 0s; waited < backoff && !stop; waited += 1s) {
                std::this_thread::sleep_for(1s);
            }
            backoff = std::min(backoff * 2, std::chrono::seconds(60));
        } catch (const std::exception& e) {
            log_error(std::string("registro recusado: ") + e.what());
            return finish(4);
        }
    }
    if (stop) return finish(0);
    log_info("registrado em " + cfg.server + " como '" + name + "' (id " +
             std::to_string(client_id) + ")");

    std::vector<SlotInfo> slot_infos;
    for (int i = 0; i < slots; ++i) {
        SlotInfo s{i, std::nullopt};
        if (!adm.cores.empty()) s.core = adm.cores[i];
        slot_infos.push_back(s);
    }

    Runner runner(cfg, api, client_id, slot_infos, arch);
    {
        std::lock_guard lock(runner_mutex);
        runner_ptr = &runner;
    }
    const bool banned = runner.run(stop);
    {
        std::lock_guard lock(runner_mutex);
        runner_ptr = nullptr;
    }
    log_info("encerrado");
    return finish(banned ? 4 : 0);
}
