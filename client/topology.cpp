#include "client/topology.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace capi::client {

namespace fs = std::filesystem;

namespace {

#if defined(__linux__)
std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// "0-3,6,8-9" -> {0,1,2,3,6,8,9}
std::set<int> parse_cpu_list(const std::string& list) {
    std::set<int> out;
    std::stringstream ss(list);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part.empty()) continue;
        const auto dash = part.find('-');
        try {
            if (dash == std::string::npos) {
                out.insert(std::stoi(part));
            } else {
                for (int i = std::stoi(part.substr(0, dash)); i <= std::stoi(part.substr(dash + 1));
                     ++i) {
                    out.insert(i);
                }
            }
        } catch (const std::exception&) {
        }
    }
    return out;
}
#endif

std::string os_string() {
    utsname u{};
    uname(&u);
    std::string os = std::string(u.sysname) + " " + u.release;
    std::ifstream rel("/etc/os-release");
    for (std::string line; std::getline(rel, line);) {
        if (line.starts_with("PRETTY_NAME=")) {
            auto v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
            os += " (" + v + ")";
        }
    }
    return os;
}

#if defined(__linux__)
Topology detect_linux() {
    Topology t;
    t.os = os_string();
    const fs::path sys = "/sys/devices/system/cpu";

    std::set<int> online = parse_cpu_list(read_file(sys / "online"));
    if (online.empty()) {
        for (int i = 0; i < sysconf(_SC_NPROCESSORS_ONLN); ++i) online.insert(i);
    }
    t.nproc = static_cast<int>(online.size());

    // Classe de cada núcleo: capacidade (ARM big.LITTLE) ou P/E (Intel híbrido).
    // Diferenças só de frequência máxima (ex: Turbo Boost Max 3.0) não contam:
    // a microarquitetura é a mesma.
    std::map<int, long> klass;
    const std::set<int> intel_p = parse_cpu_list(read_file("/sys/devices/cpu_core/cpus"));
    const std::set<int> intel_e = parse_cpu_list(read_file("/sys/devices/cpu_atom/cpus"));
    for (int cpu : online) {
        const auto cap = read_file(sys / ("cpu" + std::to_string(cpu)) / "cpu_capacity");
        if (!intel_e.empty()) klass[cpu] = intel_p.contains(cpu) ? 1 : 0;
        else if (!cap.empty()) klass[cpu] = std::stol(cap);
        else klass[cpu] = 1;
    }
    long best = 0;
    std::map<long, int> per_class;
    for (auto [cpu, k] : klass) {
        best = std::max(best, k);
        ++per_class[k];
    }
    t.heterogeneous = per_class.size() > 1;

    // Um lógico por núcleo físico da classe mais rápida.
    std::set<std::pair<std::string, std::string>> seen_cores;
    for (int cpu : online) {
        if (klass[cpu] != best) continue;
        const auto topo = sys / ("cpu" + std::to_string(cpu)) / "topology";
        const auto key = std::make_pair(read_file(topo / "physical_package_id"),
                                        read_file(topo / "core_id"));
        if (!key.second.empty() && !seen_cores.insert(key).second) continue;  // irmão SMT
        t.fast_cores.push_back(cpu);
    }

    if (t.heterogeneous) {
        std::ostringstream d;
        int i = 0;
        for (auto it = per_class.rbegin(); it != per_class.rend(); ++it, ++i) {
            d << (i ? "+" : "") << it->second << (i == 0 ? "P" : i == 1 ? "E" : "L");
        }
        t.description = d.str();
    } else {
        t.description = std::to_string(t.fast_cores.size()) + " núcleos homogêneos";
        if (static_cast<int>(t.fast_cores.size()) != t.nproc) {
            t.description += " (" + std::to_string(t.nproc) + " lógicos, SMT)";
        }
    }

    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string model_name, model;
    for (std::string line; std::getline(cpuinfo, line);) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = line.substr(0, colon);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        auto val = line.substr(colon + 1);
        if (!val.empty() && val.front() == ' ') val.erase(0, 1);
        if (key == "model name" && model_name.empty()) model_name = val;
        if (key == "Model" && model.empty()) model = val;
    }
    t.cpu_model = !model_name.empty() ? model_name : model;

    cpu_set_t mask;
    t.pinning_available = sched_getaffinity(0, sizeof(mask), &mask) == 0;
    return t;
}
#endif

#if defined(__APPLE__)
std::optional<long long> sysctl_int(const char* name) {
    long long v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return std::nullopt;
    if (len == sizeof(int)) return static_cast<long long>(*reinterpret_cast<int*>(&v));
    return v;
}

std::string sysctl_str(const char* name) {
    char buf[256] = {};
    size_t len = sizeof(buf);
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return "";
    return buf;
}

Topology detect_macos() {
    Topology t;
    t.os = os_string() + " (macOS)";
    t.cpu_model = sysctl_str("machdep.cpu.brand_string");
    t.nproc = static_cast<int>(sysctl_int("hw.logicalcpu").value_or(1));
    // macOS não tem afinidade garantida (thread_affinity_policy é só dica e é
    // ignorada no Apple Silicon).
    t.pinning_available = false;

    const auto levels = sysctl_int("hw.nperflevels").value_or(1);
    t.heterogeneous = levels > 1;
    auto fast = sysctl_int("hw.physicalcpu").value_or(t.nproc);
    if (t.heterogeneous) {
        fast = sysctl_int("hw.perflevel0.physicalcpu").value_or(0);
        const auto e = sysctl_int("hw.perflevel1.physicalcpu").value_or(0);
        t.description = std::to_string(fast) + "P+" + std::to_string(e) + "E";
        // O escalonador do Apple Silicon põe threads de QoS padrão ou maior
        // nos P-cores enquanto há folga neles (a thread principal de todo
        // processo novo já nasce user-interactive).
        t.qos_hint_available = fast > 0;
    } else {
        t.description = std::to_string(fast) + " núcleos homogêneos";
    }
    // Sem afinidade os ids não são usados; só a contagem importa.
    for (int i = 0; i < fast; ++i) t.fast_cores.push_back(i);
    return t;
}
#endif

}  // namespace

Topology detect_topology() {
#if defined(__linux__)
    return detect_linux();
#elif defined(__APPLE__)
    return detect_macos();
#else
    Topology t;
    t.os = os_string();
    t.description = "sistema não suportado";
    return t;
#endif
}

int default_slots(const Topology& t) {
    int slots = std::max(1, t.nproc / 4);
    if (t.heterogeneous && !t.pinning_available && t.qos_hint_available) {
        slots = std::min(slots, std::max(1, static_cast<int>(t.fast_cores.size()) -
                                                kQosHintReservedCores));
    }
    return slots;
}

Admission decide_admission(const Topology& t, int slots,
                           const std::optional<std::vector<int>>& requested_cores) {
    Admission a;
    if (slots < 1) {
        a.error = "slots deve ser >= 1";
        return a;
    }

    if (!t.pinning_available) {
        if (t.heterogeneous) {
            if (!t.qos_hint_available) {
                a.error = "núcleos heterogêneos (" + t.description +
                          ") e o SO não permite fixar processos em núcleos: partidas migrariam "
                          "entre núcleos rápidos e lentos no meio do jogo (plano §3.3). Este "
                          "host não pode ser client.";
                return a;
            }
            if (requested_cores) {
                a.error = "cores= exige afinidade de CPU, indisponível neste SO";
                return a;
            }
            const int max_slots = std::max(
                1, static_cast<int>(t.fast_cores.size()) - kQosHintReservedCores);
            if (slots > max_slots) {
                a.error = "modo degradado qos_hint: no máximo " + std::to_string(max_slots) +
                          " slots em " + t.description + " (núcleos rápidos − " +
                          std::to_string(kQosHintReservedCores) +
                          ", para o escalonador não precisar usar os núcleos lentos)";
                return a;
            }
            a.ok = true;
            a.pinning_mode = "qos_hint";
            a.core_topology = t.description + ", qos_hint (sem afinidade; " +
                              std::to_string(slots) + " de no máx. " + std::to_string(max_slots) +
                              " slots)";
            return a;
        }
        if (requested_cores) {
            a.error = "cores= exige afinidade de CPU, indisponível neste SO";
            return a;
        }
        if (slots > static_cast<int>(t.fast_cores.size())) {
            a.error = "slots (" + std::to_string(slots) + ") excede os " +
                      std::to_string(t.fast_cores.size()) + " núcleos físicos";
            return a;
        }
        a.ok = true;
        a.pinning_mode = "homogeneous_unpinned";
        a.core_topology = t.description + ", sem afinidade";
        return a;
    }

    std::vector<int> cores;
    if (requested_cores) {
        cores = *requested_cores;
        for (int c : cores) {
            if (std::find(t.fast_cores.begin(), t.fast_cores.end(), c) == t.fast_cores.end()) {
                a.error = "núcleo " + std::to_string(c) +
                          " não é um núcleo físico da classe mais rápida (disponíveis: " +
                          [&] {
                              std::string s;
                              for (int f : t.fast_cores) s += (s.empty() ? "" : ",") + std::to_string(f);
                              return s;
                          }() + ")";
                return a;
            }
        }
        if (static_cast<int>(cores.size()) != slots ||
            std::set<int>(cores.begin(), cores.end()).size() != cores.size()) {
            a.error = "cores= deve listar exatamente " + std::to_string(slots) +
                      " núcleos distintos (um por slot)";
            return a;
        }
    } else {
        if (slots > static_cast<int>(t.fast_cores.size())) {
            a.error = "slots (" + std::to_string(slots) + ") excede os " +
                      std::to_string(t.fast_cores.size()) + " núcleos rápidos disponíveis (" +
                      t.description + ")";
            return a;
        }
        // Os de maior índice: o núcleo 0 costuma atender mais interrupções.
        cores.assign(t.fast_cores.end() - slots, t.fast_cores.end());
    }

    a.ok = true;
    a.pinning_mode = "pinned";
    a.cores = cores;
    std::string list;
    for (int c : cores) list += (list.empty() ? "" : ",") + std::to_string(c);
    a.core_topology = t.description + ", pinado em " + list;
    return a;
}

bool verify_pinning(const std::vector<int>& cores, std::string& error) {
#if defined(__linux__)
    for (int core : cores) {
        bool ok = false;
        std::thread th([&] {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(core, &set);
            if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) return;
            sched_yield();
            ok = sched_getcpu() == core;
        });
        th.join();
        if (!ok) {
            error = "não foi possível fixar uma thread no núcleo " + std::to_string(core);
            return false;
        }
    }
    return true;
#else
    (void)cores;
    error = "afinidade de CPU não suportada neste SO";
    return false;
#endif
}

}  // namespace capi::client
