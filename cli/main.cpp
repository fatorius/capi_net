// capi_net — CLI de submissão e acompanhamento de testes (plano §5.8).
// Camada fina sobre a API HTTP do server.

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;

namespace {

const char* kUsage = R"(uso: capi_net [--server URL] <comando> [opções]

comandos:
  submit --candidate REF --baseline REF --tc BASE+INC --book ARQUIVO --pairs N
         [--preset gainer|nonreg|custom] [--elo0 X --elo1 Y [--alpha A] [--beta B]]
         [--kind sprt|gauntlet] [--name NOME] [--hash MB] [--priority N] [--wait]
  list [--limit N]              testes e status
  status <test_id>              LLR, Elo, penta, progresso
  validation <test_id>          log do smoke test
  stop <test_id>                parada manual
  priority <test_id> <n>        reordenar a fila

servidor: --server, ou a variável CAPI_SERVER (padrão http://localhost:8080)
)";

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    std::set<std::string> flags;
};

Args parse_args(int argc, char** argv) {
    static const std::set<std::string> kFlags{"wait", "help"};
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h") arg = "--help";
        if (arg.starts_with("--")) {
            const auto key = arg.substr(2);
            if (kFlags.contains(key)) {
                a.flags.insert(key);
            } else if (i + 1 < argc) {
                a.options[key] = argv[++i];
            } else {
                throw std::runtime_error("faltou o valor de " + arg);
            }
        } else {
            a.positional.push_back(arg);
        }
    }
    return a;
}

struct Api {
    httplib::Client client;
    std::string base;

    explicit Api(const std::string& url) : client(url), base(url) {
        client.set_connection_timeout(5);
        client.set_read_timeout(90);  // criação de teste consulta o GitHub e conta o book
    }

    json call(const std::string& method, const std::string& path,
              const std::optional<json>& body = std::nullopt) {
        const std::string payload = body ? body->dump() : "";
        httplib::Result r = method == "GET"    ? client.Get(path)
                            : method == "POST" ? client.Post(path, payload, "application/json")
                                               : client.Patch(path, payload, "application/json");
        if (!r) {
            throw std::runtime_error("sem resposta de " + base + path + " (" +
                                     httplib::to_string(r.error()) + ")");
        }
        json j = r->body.empty() ? json() : json::parse(r->body, nullptr, false);
        if (r->status >= 400) {
            std::string msg = "HTTP " + std::to_string(r->status);
            if (j.is_object()) {
                if (j.contains("error")) msg += ": " + j["error"].get<std::string>();
                if (j.contains("reason")) msg += " — " + j["reason"].get<std::string>();
            }
            throw std::runtime_error(msg);
        }
        return j;
    }
};

const std::string& need(const Args& a, const std::string& key) {
    auto it = a.options.find(key);
    if (it == a.options.end()) throw std::runtime_error("faltou --" + key);
    return it->second;
}

long long to_int(const std::string& s, const std::string& what) {
    try {
        std::size_t pos = 0;
        const long long v = std::stoll(s, &pos);
        if (pos == s.size()) return v;
    } catch (const std::exception&) {
    }
    throw std::runtime_error(what + " inválido: " + s);
}

double to_double(const std::string& s, const std::string& what) {
    try {
        std::size_t pos = 0;
        const double v = std::stod(s, &pos);
        if (pos == s.size()) return v;
    } catch (const std::exception&) {
    }
    throw std::runtime_error(what + " inválido: " + s);
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

std::string opt_num(const json& v, const char* f) {
    return v.is_number() ? fmt(f, v.get<double>()) : "-";
}

std::string tc_string(int base_ms, int inc_ms) {
    return fmt("%g", base_ms / 1000.0) + "+" + fmt("%g", inc_ms / 1000.0);
}

std::string short_sha(const json& side) {
    return side["commit"].get<std::string>().substr(0, 7);
}

// ---------------------------------------------------------------------------
// comandos
// ---------------------------------------------------------------------------
int cmd_validation(Api& api, long long id, bool quiet_if_ok);

int cmd_submit(Api& api, const Args& a) {
    json body{{"candidate_ref", need(a, "candidate")},
              {"baseline_ref", need(a, "baseline")},
              {"tc", need(a, "tc")},
              {"book_name", need(a, "book")},
              {"total_pairs", to_int(need(a, "pairs"), "--pairs")}};

    const std::string kind = a.options.contains("kind") ? a.options.at("kind") : "sprt";
    body["kind"] = kind;
    if (kind == "sprt") {
        body["preset"] = a.options.contains("preset") ? a.options.at("preset") : "gainer";
    } else if (a.options.contains("preset")) {
        body["preset"] = a.options.at("preset");  // o server rejeita com a mensagem certa
    }
    for (const auto& [opt, field] : {std::pair{"elo0", "sprt_elo0"}, {"elo1", "sprt_elo1"},
                                     {"alpha", "sprt_alpha"}, {"beta", "sprt_beta"}}) {
        if (a.options.contains(opt)) body[field] = to_double(a.options.at(opt), "--" + std::string(opt));
    }
    if (a.options.contains("name")) body["name"] = a.options.at("name");
    if (a.options.contains("hash")) body["hash_mb"] = to_int(a.options.at("hash"), "--hash");
    if (a.options.contains("priority")) {
        body["priority"] = to_int(a.options.at("priority"), "--priority");
    }
    if (const char* user = std::getenv("USER")) body["submitted_by"] = user;

    const auto r = api.call("POST", "/api/admin/tests", body);
    const long long id = r["test_id"].get<long long>();
    std::cout << "teste " << id << " criado (" << r["status"].get<std::string>() << ")\n"
              << "  candidate: " << r["candidate"]["ref"].get<std::string>() << " → "
              << r["candidate"]["commit"].get<std::string>() << "\n"
              << "  baseline:  " << r["baseline"]["ref"].get<std::string>() << " → "
              << r["baseline"]["commit"].get<std::string>() << "\n";

    if (!a.flags.contains("wait")) {
        std::cout << "acompanhe com: capi_net status " << id << "\n";
        return 0;
    }
    std::cout << "aguardando o smoke test..." << std::flush;
    while (true) {
        const auto v = api.call("GET", "/api/admin/tests/" + std::to_string(id) + "/validation");
        if (v["status"] != "validating") break;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "." << std::flush;
    }
    std::cout << "\n";
    return cmd_validation(api, id, /*quiet_if_ok=*/true);
}

int cmd_validation(Api& api, long long id, bool quiet_if_ok) {
    const auto v = api.call("GET", "/api/admin/tests/" + std::to_string(id) + "/validation");
    const auto status = v["status"].get<std::string>();
    std::cout << "teste " << id << ": " << status << "\n";
    const bool failed = status == "invalid";
    if (v["validation_log"].is_string() && (failed || !quiet_if_ok)) {
        std::cout << v["validation_log"].get<std::string>();
    }
    return failed ? 1 : 0;
}

int cmd_list(Api& api, const Args& a) {
    std::string path = "/api/tests";
    if (a.options.contains("limit")) {
        path += "?limit=" + std::to_string(to_int(a.options.at("limit"), "--limit"));
    }
    const auto tests = api.call("GET", path);
    std::printf("%5s  %-10s  %-12s  %4s  %-28s  %-19s  %8s  %8s  %13s\n", "id", "status",
                "result", "prio", "name", "candidate/baseline", "LLR", "Elo", "pares");
    for (const auto& t : tests) {
        std::string name = t["name"].get<std::string>();
        if (name.size() > 28) name = name.substr(0, 27) + "…";
        const std::string shas = short_sha(t["candidate"]) + "/" + short_sha(t["baseline"]);
        const std::string pairs = std::to_string(t["pairs_valid"].get<int>()) + "/" +
                                  std::to_string(t["total_pairs"].get<int>());
        std::printf("%5lld  %-10s  %-12s  %4d  %-28s  %-19s  %8s  %8s  %13s\n",
                    t["test_id"].get<long long>(), t["status"].get<std::string>().c_str(),
                    t["result"].get<std::string>().c_str(), t["priority"].get<int>(),
                    name.c_str(), shas.c_str(), opt_num(t["llr"], "%.2f").c_str(),
                    opt_num(t["elo"], "%+.2f").c_str(), pairs.c_str());
    }
    return 0;
}

int cmd_status(Api& api, long long id) {
    const auto s = api.call("GET", "/api/tests/" + std::to_string(id) + "/stats");
    const auto& p = s["pairs"];
    const auto& pe = s["penta"];
    const auto& e = s["elo"];

    std::cout << "#" << id << " " << s["name"].get<std::string>() << "  ["
              << s["status"].get<std::string>() << "]  result: "
              << s["result"].get<std::string>() << "\n";
    std::cout << "  candidate  " << s["candidate"]["ref"].get<std::string>() << " @ "
              << s["candidate"]["commit"].get<std::string>() << "\n";
    std::cout << "  baseline   " << s["baseline"]["ref"].get<std::string>() << " @ "
              << s["baseline"]["commit"].get<std::string>() << "\n";
    std::cout << "  tc " << tc_string(s["tc_base_ms"], s["tc_increment_ms"]) << " · hash "
              << s["hash_mb"].get<int>() << " MB · threads " << s["threads"].get<int>()
              << " · book " << s["book_name"].get<std::string>() << " · prioridade "
              << s["priority"].get<int>() << "\n";
    std::cout << "  pares      " << p["valid"].get<long long>() << " válidos / "
              << p["total"].get<int>() << "  (pending " << p["pending"].get<int>() << ", leased "
              << p["leased"].get<int>() << ", completed " << p["completed"].get<int>()
              << ", discarded " << p["discarded"].get<int>() << ")\n";
    std::cout << "  penta      LL " << pe["ll"].get<long long>() << " · LD "
              << pe["ld"].get<long long>() << " · DD/WL " << pe["dd_wl"].get<long long>()
              << " · WD " << pe["wd"].get<long long>() << " · WW " << pe["ww"].get<long long>()
              << "\n";
    std::cout << "  Elo        " << opt_num(e["value"], "%+.2f") << "  ["
              << opt_num(e["ci_low"], "%+.2f") << ", " << opt_num(e["ci_high"], "%+.2f")
              << "] (95%, logístico)\n";
    if (s.contains("adjudication")) {
        const auto& adj = s["adjudication"];
        std::cout << "  adjudicação ";
        if (adj["draw"].is_object()) {
            std::cout << "empate a partir do lance " << adj["draw"]["movenumber"].get<int>()
                      << ", |eval| ≤ " << adj["draw"]["score_cp"].get<int>() << " cp por "
                      << adj["draw"]["movecount"].get<int>() << " lances";
        } else {
            std::cout << "empate desligada";
        }
        if (adj["resign"].is_object()) {
            std::cout << " · vitória com |eval| ≥ " << adj["resign"]["score_cp"].get<int>()
                      << " cp por " << adj["resign"]["movecount"].get<int>() << " lances"
                      << (adj["resign"]["twosided"].get<bool>() ? " (as duas engines)" : "");
        } else {
            std::cout << " · vitória desligada";
        }
        std::cout << "\n";
    }
    if (s.contains("sprt")) {
        const auto& sp = s["sprt"];
        std::cout << "  LLR        " << opt_num(sp["llr"], "%.2f") << "  ["
                  << opt_num(sp["llr_lower"], "%.2f") << ", " << opt_num(sp["llr_upper"], "%.2f")
                  << "]  bounds nElo [" << opt_num(sp["elo0"], "%g") << ", "
                  << opt_num(sp["elo1"], "%g") << "], α " << opt_num(sp["alpha"], "%g")
                  << ", β " << opt_num(sp["beta"], "%g") << "\n";
    }
    if (s["status"] == "validating" || s["status"] == "invalid") {
        std::cout << "\n";
        cmd_validation(api, id, /*quiet_if_ok=*/false);
    }
    return 0;
}

int run(int argc, char** argv) {
    const Args a = parse_args(argc, argv);
    if (a.flags.contains("help") || a.positional.empty()) {
        std::cout << kUsage;
        return a.positional.empty() && !a.flags.contains("help") ? 2 : 0;
    }

    std::string server = "http://localhost:8080";
    if (const char* env = std::getenv("CAPI_SERVER")) server = env;
    if (a.options.contains("server")) server = a.options.at("server");
    Api api(server);

    const auto& cmd = a.positional[0];
    auto id_arg = [&](std::size_t i) {
        if (a.positional.size() <= i) throw std::runtime_error("faltou o test_id");
        return to_int(a.positional[i], "test_id");
    };

    if (cmd == "submit") return cmd_submit(api, a);
    if (cmd == "list") return cmd_list(api, a);
    if (cmd == "status") return cmd_status(api, id_arg(1));
    if (cmd == "validation") return cmd_validation(api, id_arg(1), /*quiet_if_ok=*/false);
    if (cmd == "stop") {
        const auto r = api.call("POST", "/api/admin/tests/" + std::to_string(id_arg(1)) + "/stop");
        std::cout << "teste " << id_arg(1) << " parado (estava "
                  << r["previous_status"].get<std::string>() << ")\n";
        if (r["promoted_test_id"].is_number()) {
            std::cout << "teste " << r["promoted_test_id"].get<long long>()
                      << " promovido para running\n";
        }
        return 0;
    }
    if (cmd == "priority") {
        if (a.positional.size() < 3) throw std::runtime_error("uso: capi_net priority <id> <n>");
        const auto n = to_int(a.positional[2], "prioridade");
        api.call("PATCH", "/api/admin/tests/" + std::to_string(id_arg(1)), json{{"priority", n}});
        std::cout << "teste " << id_arg(1) << ": prioridade " << n << "\n";
        return 0;
    }
    throw std::runtime_error("comando desconhecido: " + cmd + "\n\n" + kUsage);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "capi_net: " << e.what() << "\n";
        return 1;
    }
}
