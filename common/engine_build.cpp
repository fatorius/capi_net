#include "common/engine_build.hpp"

#include "common/process.hpp"

#include <chrono>
#include <regex>
#include <sstream>

namespace capi::build {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kLogTailLines = 30;

std::string join(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& a : args) out += (out.empty() ? "" : " ") + a;
    return out;
}

// Roda um passo e registra comando, código de saída e duração; em falha,
// anexa o final da saída.
bool step(std::string& log, const std::vector<std::string>& argv, proc::ProcessOptions opts,
          proc::ProcessResult* out = nullptr) {
    const auto start = std::chrono::steady_clock::now();
    auto r = proc::run_process(argv, opts);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);

    std::ostringstream line;
    line.precision(1);
    line << std::fixed << "$ " << join(argv) << "  -> ";
    if (r.cancelled) line << "cancelled";
    else if (r.timed_out) line << "TIMEOUT";
    else line << "exit " << r.exit_code;
    line << " (" << secs.count() << "s)\n";
    log += line.str();

    const bool ok = !r.cancelled && !r.timed_out && r.exit_code == 0;
    if (!ok && !r.cancelled) log += proc::tail_lines(r.output, kLogTailLines) + "\n";
    if (out) *out = std::move(r);
    return ok;
}

}  // namespace

bool host_has_bmi2() {
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_cpu_supports("bmi2");
#else
    return false;
#endif
}

std::string host_arch_target() {
#if defined(__x86_64__) || defined(_M_X64)
    if (__builtin_cpu_supports("bmi2")) return "x86-64-bmi2";
    if (__builtin_cpu_supports("avx2")) return "x86-64-avx2";
    return "x86-64";
#elif defined(__aarch64__)
    return "armv8";
#else
    return "unknown";
#endif
}

std::optional<fs::path> build_engine(const BuildOptions& opts, std::string& log) {
    const fs::path dir = opts.work_dir / "builds" / opts.cache_key;
    const fs::path bin = dir / "capizero";
    if (fs::exists(bin)) {
        log += "cached build: " + bin.string() + "\n";
        return bin;
    }

    const fs::path tmp = opts.work_dir / "builds" / (opts.cache_key + ".tmp");
    fs::remove_all(tmp);
    fs::create_directories(tmp / "src");
    const fs::path tarball = tmp / "src.tar.gz";

    proc::ProcessOptions o;
    o.cancel = opts.cancel;
    o.timeout = 180s;
    if (!step(log, {"curl", "-fsSL", "--retry", "2", "--max-time", "150", "-o", tarball.string(),
                    opts.tarball_url},
              o)) {
        return std::nullopt;
    }
    o.timeout = 60s;
    if (!step(log, {"tar", "-xzf", tarball.string(), "-C", (tmp / "src").string(),
                    "--strip-components=1"},
              o)) {
        return std::nullopt;
    }

    std::vector<std::string> make{"make", "-C", (tmp / "src").string(), "build", "NAME=capizero"};
    bool pext_given = false;
    for (const auto& a : opts.make_args) pext_given |= a.starts_with("PEXT=");
    // PEXT é x86 (BMI2); o Makefile do capizero o usa por padrão.
    if (!pext_given && !host_has_bmi2()) make.push_back("PEXT=false");
    make.insert(make.end(), opts.make_args.begin(), opts.make_args.end());
    o.timeout = 1800s;  // Raspberry Pi com -O3 -flto
    if (!step(log, make, o)) return std::nullopt;

    fs::path built = tmp / "src" / "capizero";
    if (!fs::exists(built)) {
        // Makefiles antigos ignoram NAME e geram capizero_<versão>.
        std::vector<fs::path> found;
        for (const auto& e : fs::directory_iterator(tmp / "src")) {
            if (e.is_regular_file() && e.path().filename().string().starts_with("capizero") &&
                (fs::status(e.path()).permissions() & fs::perms::owner_exec) != fs::perms::none) {
                found.push_back(e.path());
            }
        }
        if (found.size() != 1) {
            log += "build finished but produced no single capizero* binary in " +
                   (tmp / "src").string() + "\n";
            return std::nullopt;
        }
        built = found[0];
        log += "Makefile ignored NAME; using " + built.filename().string() + "\n";
    }
    fs::create_directories(dir);
    fs::rename(built, bin);
    fs::remove_all(tmp);
    log += "built " + bin.string() + "\n";
    return bin;
}

bool check_engine(const fs::path& bin, int bench_depth, std::string& log,
                  const std::atomic<bool>* cancel) {
    proc::ProcessOptions o;
    o.cancel = cancel;
    o.timeout = 300s;
    proc::ProcessResult r;
    if (!step(log, {bin.string(), "bench", std::to_string(bench_depth)}, o, &r)) return false;
    static const std::regex nodes_re(R"(Nodes:\s*(\d+))");
    std::smatch m;
    if (!std::regex_search(r.output, m, nodes_re) || std::stoull(m[1].str()) == 0) {
        log += "bench output has no positive 'Nodes:' line:\n" +
               proc::tail_lines(r.output, kLogTailLines) + "\n";
        return false;
    }
    log += "bench ok: " + m[0].str() + "\n";

    o.timeout = 20s;
    o.stdin_data = "uci\nisready\nquit\n";
    if (!step(log, {bin.string(), "uci"}, o, &r)) return false;
    if (r.output.find("uciok") == std::string::npos ||
        r.output.find("readyok") == std::string::npos) {
        log += "engine did not answer uciok/readyok:\n" +
               proc::tail_lines(r.output, kLogTailLines) + "\n";
        return false;
    }
    log += "uci ok: uciok, readyok\n";
    return true;
}

}  // namespace capi::build
