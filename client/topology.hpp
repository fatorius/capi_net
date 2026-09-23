#pragma once

#include <optional>
#include <string>
#include <vector>

namespace capi::client {

struct Topology {
    std::string os;           // ex: "Linux 6.12 (Debian GNU/Linux 12)"
    std::string cpu_model;    // ex: "Raspberry Pi 5 Model B", "Apple M3 Pro"
    int nproc = 0;            // núcleos lógicos online
    bool heterogeneous = false;      // núcleos de tipos diferentes (P/E, big.LITTLE)
    bool pinning_available = false;  // afinidade garantida pelo SO (sched_setaffinity)
    // Sem afinidade, mas o SO prefere os núcleos rápidos enquanto houver folga
    // neles (macOS no Apple Silicon): permite o modo degradado 'qos_hint'.
    bool qos_hint_available = false;
    // Um núcleo lógico por núcleo físico da classe mais rápida (sem irmãos SMT).
    std::vector<int> fast_cores;
    std::string description;  // ex: "8P+4E", "4 núcleos homogêneos"
};

// Detecta a topologia deste host (Linux: sysfs; macOS: sysctl).
Topology detect_topology();

// Regra de admissão (plano §3.3, com o modo degradado do §12):
//   heterogêneo + pinning   -> pinned nos núcleos rápidos
//   homogêneo   + pinning   -> pinned
//   homogêneo   sem pinning -> homogeneous_unpinned
//   heterogêneo sem pinning -> qos_hint com no máximo (núcleos rápidos − 2)
//                              slots, se o SO prefere os núcleos rápidos
//                              (Apple Silicon); senão recusado.
// A folga de 2 núcleos rápidos evita que o escalonador precise usar os lentos:
// no M3 Pro, até 5 benches simultâneos (= P-cores) rodam juntos a +10%; com 7,
// todos pioram 30% (revezamento entre P e E).
struct Admission {
    bool ok = false;
    std::string pinning_mode;  // 'pinned' | 'homogeneous_unpinned'
    std::vector<int> cores;    // um por slot (vazio se unpinned)
    std::string core_topology; // texto para clients.core_topology
    std::string error;
};
Admission decide_admission(const Topology& t, int slots,
                           const std::optional<std::vector<int>>& requested_cores);

inline constexpr int kQosHintReservedCores = 2;

// Slots padrão: NPROC/4 (mínimo 1) (§3.4), limitado ao máximo do modo.
int default_slots(const Topology& t);

// Linux: confirma que cada núcleo aceita afinidade e que uma thread fixada
// nele de fato roda lá. Em outros SOs retorna false com a razão.
bool verify_pinning(const std::vector<int>& cores, std::string& error);

}  // namespace capi::client
