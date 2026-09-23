#include "client/fastchess_setup.hpp"

#include "common/process.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace capi::client {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

std::string fastchess_tarball_url(const std::string& tag) {
    return "https://codeload.github.com/Disservin/fastchess/tar.gz/refs/tags/" + tag;
}

namespace {

std::optional<std::string> probe_version(const fs::path& bin, std::string& log) {
    proc::ProcessOptions o;
    o.timeout = 10s;
    const auto r = proc::run_process({bin.string(), "-version"}, o);
    if (r.exit_code != 0) {
        log += bin.string() + " -version falhou (código " + std::to_string(r.exit_code) + "): " +
               proc::tail_lines(r.output, 3) + "\n";
        return std::nullopt;
    }
    auto line = r.output.substr(0, r.output.find('\n'));
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

bool step(std::string& log, const std::vector<std::string>& argv, proc::ProcessOptions o) {
    std::string cmd;
    for (const auto& a : argv) cmd += (cmd.empty() ? "" : " ") + a;
    const auto r = proc::run_process(argv, o);
    const bool ok = r.exit_code == 0 && !r.timed_out && !r.cancelled;
    log += "$ " + cmd + "  -> " +
           (r.cancelled ? std::string("cancelled")
                        : r.timed_out ? std::string("TIMEOUT") : "exit " + std::to_string(r.exit_code)) +
           "\n";
    if (!ok && !r.cancelled) log += proc::tail_lines(r.output, 20) + "\n";
    return ok;
}

}  // namespace

std::optional<FastchessInfo> ensure_fastchess(const std::string& configured,
                                              const fs::path& work_dir, std::string& log,
                                              const std::atomic<bool>* cancel) {
    if (!configured.empty()) {
        const auto v = probe_version(configured, log);
        if (!v) return std::nullopt;
        return FastchessInfo{configured, *v};
    }

    const fs::path dir = work_dir / "tools" / (std::string("fastchess-") + kFastchessTag);
    const fs::path bin = dir / "fastchess";
    if (fs::exists(bin)) {
        if (const auto v = probe_version(bin, log)) return FastchessInfo{bin, *v};
        log += "binário em cache inválido; recompilando\n";
        fs::remove_all(dir);
    }

    const fs::path tmp = work_dir / "tools" / (std::string("fastchess-") + kFastchessTag + ".tmp");
    fs::remove_all(tmp);
    fs::create_directories(tmp / "src");
    const fs::path tarball = tmp / "src.tar.gz";

    proc::ProcessOptions o;
    o.cancel = cancel;
    o.timeout = 180s;
    if (!step(log, {"curl", "-fsSL", "--retry", "2", "--max-time", "150", "-o", tarball.string(),
                    fastchess_tarball_url(kFastchessTag)},
              o)) {
        return std::nullopt;
    }
    o.timeout = 60s;
    if (!step(log, {"tar", "-xzf", tarball.string(), "-C", (tmp / "src").string(),
                    "--strip-components=1"},
              o)) {
        return std::nullopt;
    }
    // -j2: o Raspberry Pi tem pouca memória para mais jobs em paralelo.
    const unsigned jobs = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 2u);
    o.timeout = 3600s;
    if (!step(log, {"make", "-C", (tmp / "src").string(), "-j" + std::to_string(jobs)}, o)) {
        return std::nullopt;
    }

    const fs::path built = tmp / "src" / "fastchess";
    if (!fs::exists(built)) {
        log += "make terminou mas não gerou " + built.string() + "\n";
        return std::nullopt;
    }
    fs::create_directories(dir);
    fs::rename(built, bin);
    fs::remove_all(tmp);
    const auto v = probe_version(bin, log);
    if (!v) return std::nullopt;
    log += "compilado " + bin.string() + " (" + *v + ")\n";
    return FastchessInfo{bin, *v};
}

}  // namespace capi::client
