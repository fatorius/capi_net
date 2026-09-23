#include "client/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace capi::client {

namespace fs = std::filesystem;

const char* kClientUsage = R"(uso: capi_net_client [opções]

  --config ARQ        arquivo de configuração (padrão: ~/.config/capi_net/client.conf)
  --server URL        URL do server (ex: http://<host-do-server>:8080)
  --name NOME         nome do client (padrão: <hostname>-<N>slots)
  --slots N           partidas simultâneas (padrão: NPROC/4)
  --cores LISTA       núcleos, um por slot (ex: 2,3)
  --work-dir DIR      builds e arquivos temporários (padrão: ~/.cache/capi_net)
  --fastchess CMD     binário do fastchess (padrão: compila a versão fixa do capi_net)
  --make-args "A B"   argumentos extras para `make build`
  --poll-interval S   intervalo de consulta ao server, em segundos (padrão: 10)
  --max-pairs N       sai depois de N pares (testes)
  --check             mostra topologia e admissão e sai
)";

namespace {

std::string expand_home(const std::string& p) {
    if (p.starts_with("~/")) {
        if (const char* home = std::getenv("HOME")) return std::string(home) + p.substr(1);
    }
    return p;
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

int to_int(const std::string& key, const std::string& v, int min) {
    try {
        std::size_t pos = 0;
        const int n = std::stoi(v, &pos);
        if (pos == v.size() && n >= min) return n;
    } catch (const std::exception&) {
    }
    throw ConfigError(key + ": valor inválido '" + v + "'");
}

void apply(ClientConfig& c, const std::string& key, const std::string& value) {
    if (key == "server") c.server = value;
    else if (key == "name") c.name = value;
    else if (key == "slots") c.slots = to_int(key, value, 1);
    else if (key == "cores") {
        std::vector<int> cores;
        std::stringstream ss(value);
        for (std::string part; std::getline(ss, part, ',');) {
            cores.push_back(to_int(key, trim(part), 0));
        }
        if (cores.empty()) throw ConfigError("cores: lista vazia");
        c.cores = cores;
    } else if (key == "work_dir" || key == "work-dir") c.work_dir = expand_home(value);
    else if (key == "fastchess") c.fastchess = expand_home(value);
    else if (key == "make_args" || key == "make-args") {
        c.make_args.clear();
        std::stringstream ss(value);
        for (std::string a; ss >> a;) c.make_args.push_back(a);
    } else if (key == "poll_interval" || key == "poll-interval") {
        c.poll_interval = std::chrono::seconds(to_int(key, value, 1));
    } else if (key == "max_pairs" || key == "max-pairs") c.max_pairs = to_int(key, value, 0);
    else throw ConfigError("opção desconhecida: " + key);
}

void load_file(ClientConfig& c, const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw ConfigError("não foi possível ler " + path.string());
    int lineno = 0;
    for (std::string line; std::getline(in, line);) {
        ++lineno;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            throw ConfigError(path.string() + ":" + std::to_string(lineno) + ": esperado chave = valor");
        }
        try {
            apply(c, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
        } catch (const ConfigError& e) {
            throw ConfigError(path.string() + ":" + std::to_string(lineno) + ": " + e.what());
        }
    }
}

}  // namespace

ClientConfig load_client_config(int argc, char** argv) {
    std::map<std::string, std::string> flags;
    std::optional<std::string> config_path;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") throw ConfigError(kClientUsage);
        if (arg == "--check") {
            check = true;
            continue;
        }
        if (!arg.starts_with("--")) throw ConfigError("argumento inesperado: " + arg);
        if (i + 1 >= argc) throw ConfigError("faltou o valor de " + arg);
        const auto key = arg.substr(2);
        if (key == "config") config_path = expand_home(argv[++i]);
        else flags[key] = argv[++i];
    }

    ClientConfig c;
    const std::string default_path = expand_home("~/.config/capi_net/client.conf");
    if (config_path) load_file(c, *config_path);
    else if (fs::exists(default_path)) load_file(c, default_path);
    for (const auto& [k, v] : flags) apply(c, k, v);

    c.check = check;
    if (c.work_dir.empty()) c.work_dir = expand_home("~/.cache/capi_net");
    while (c.server.size() > 1 && c.server.back() == '/') c.server.pop_back();
    return c;
}

}  // namespace capi::client
