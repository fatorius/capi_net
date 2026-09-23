#include "client/runner.hpp"

#include "client/pgn.hpp"
#include "common/engine_build.hpp"
#include "common/log.hpp"
#include "common/process.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace capi::client {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Partes puras
// ---------------------------------------------------------------------------
EffectiveTc effective_tc(const EngineSet& e, double cpu_factor) {
    return {static_cast<int>(std::lround(e.tc_base_ms * cpu_factor)),
            static_cast<int>(std::lround(e.tc_increment_ms * cpu_factor))};
}

std::string fastchess_tc(const EffectiveTc& tc) {
    auto secs = [](int ms) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", ms / 1000.0);
        std::string s = buf;
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
        return s;
    };
    return secs(tc.base_ms) + "+" + secs(tc.increment_ms);
}

std::vector<std::string> fastchess_args(const std::string& fastchess, const EngineSet& e,
                                        const EffectiveTc& tc, const fs::path& epd,
                                        const fs::path& pgn, const std::string& event) {
    std::vector<std::string> args{fastchess,
            "-engine", "cmd=" + e.candidate.string(), "name=candidate", "args=uci",
            "-engine", "cmd=" + e.baseline.string(), "name=baseline", "args=uci",
            "-each", "tc=" + fastchess_tc(tc), "option.Hash=" + std::to_string(e.hash_mb),
            "option.Threads=" + std::to_string(e.threads),
            "-openings", "file=" + epd.string(), "format=epd", "order=sequential",
            "-rounds", "1", "-games", "2", "-repeat", "-concurrency", "1",
            "-pgnout", "file=" + pgn.string(), "notation=san", "append=false",
            "-event", event, "-ratinginterval", "0"};
    const auto adj = adjudication_args(e.adjudication);
    args.insert(args.end(), adj.begin(), adj.end());
    return args;
}

std::vector<std::string> adjudication_args(const Adjudication& a) {
    std::vector<std::string> out;
    if (a.draw) {
        out.insert(out.end(), {"-draw", "movenumber=" + std::to_string(a.draw->movenumber),
                               "movecount=" + std::to_string(a.draw->movecount),
                               "score=" + std::to_string(a.draw->score_cp)});
    }
    if (a.resign) {
        out.insert(out.end(), {"-resign", "movecount=" + std::to_string(a.resign->movecount),
                               "score=" + std::to_string(a.resign->score_cp),
                               std::string("twosided=") + (a.resign->twosided ? "true" : "false")});
    }
    return out;
}

json make_result_body(std::int64_t client_id, const std::string& pgn,
                      const std::string& failure_note, const SlotInfo& slot,
                      const EffectiveTc& tc, double cpu_factor) {
    std::vector<std::optional<ParsedGame>> parsed;
    for (const auto& text : split_pgn_games(pgn)) {
        if (parsed.size() == 2) break;
        parsed.push_back(parse_game(text, "candidate"));
    }
    parsed.resize(2);

    json games = json::array();
    for (int i = 0; i < 2; ++i) {
        json g{{"game_in_pair", i},
               {"cpu_factor", cpu_factor},
               {"tc_base_effective_ms", tc.base_ms},
               {"tc_increment_effective_ms", tc.increment_ms},
               {"slot_index", slot.index},
               {"core_id", slot.core ? json(*slot.core) : json(nullptr)}};
        if (const auto& p = parsed[i]) {
            g["candidate_is_white"] = p->candidate_is_white;
            g["outcome"] = p->outcome;
            g["termination"] = p->termination;
            g["ply_count"] = p->ply_count ? json(*p->ply_count) : json(nullptr);
            g["duration_ms"] = p->duration_ms ? json(*p->duration_ms) : json(nullptr);
            g["pgn"] = p->text;
        } else {
            const auto& other = parsed[1 - i];
            g["candidate_is_white"] = other ? !other->candidate_is_white : i == 0;
            g["outcome"] = "draw";
            g["termination"] = "crash";
            g["ply_count"] = nullptr;
            g["duration_ms"] = nullptr;
            g["pgn"] = "{capi_net: partida ausente no PGN do fastchess. " + failure_note + "}\n";
        }
        games.push_back(std::move(g));
    }
    return {{"client_id", client_id}, {"games", games}};
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------
Runner::Runner(const ClientConfig& cfg, Api& api, std::int64_t client_id,
               std::vector<SlotInfo> slots, std::string arch_target)
    : cfg_(cfg), api_(api), client_id_(client_id), slots_(std::move(slots)),
      arch_target_(std::move(arch_target)) {}

void Runner::wake() { cv_.notify_all(); }

void Runner::request_stop() {
    *stop_ = true;
    wake();
}

void Runner::sleep_for(std::chrono::milliseconds d) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, d, [&] { return stop_->load(); });
}

std::shared_ptr<const EngineSet> Runner::engines() {
    std::lock_guard lock(mutex_);
    return engines_;
}

bool Runner::run(std::atomic<bool>& stop) {
    stop_ = &stop;
    std::vector<std::thread> threads;
    for (const auto& s : slots_) threads.emplace_back([this, s] { slot_loop(s); });

    while (!stop) {
        try {
            poll_server();
        } catch (const TransientError& e) {
            log_warn(std::string("server indisponível: ") + e.what());
        } catch (const std::exception& e) {
            log_error(std::string("poll: ") + e.what());
        }
        sleep_for(cfg_.poll_interval);
    }

    wake();
    for (auto& t : threads) t.join();
    return banned_;
}

void Runner::poll_server() {
    if (api_.heartbeat(client_id_, cpu_factor_)) {
        log_error("server informou que este client foi banido; encerrando");
        banned_ = true;
        request_stop();
        return;
    }

    const auto t = api_.active_test();
    const auto current = engines();
    if (!t) {
        if (current) log_info("nenhum teste ativo; aguardando");
        std::lock_guard lock(mutex_);
        engines_.reset();
        return;
    }
    if (current && current->test_id == t->test_id) return;
    if (failed_test_ == t->test_id) return;

    {
        std::lock_guard lock(mutex_);
        engines_.reset();  // slots param de pedir pares do teste antigo
    }
    if (prepare_engines(*t)) {
        wake();
    } else {
        failed_test_ = t->test_id;
    }
}

bool Runner::prepare_engines(const ActiveTest& t) {
    log_info("teste " + std::to_string(t.test_id) + " ativo: preparando engines (" +
             t.candidate_commit.substr(0, 7) + " vs " + t.baseline_commit.substr(0, 7) + ")");
    auto set = std::make_shared<EngineSet>();
    set->test_id = t.test_id;
    set->tc_base_ms = t.tc_base_ms;
    set->tc_increment_ms = t.tc_increment_ms;
    set->hash_mb = t.hash_mb;
    set->threads = t.threads;
    set->adjudication = t.adjudication;

    for (const auto& [sha, url, dest] :
         {std::tuple{t.candidate_commit, t.candidate_tarball_url, &set->candidate},
          std::tuple{t.baseline_commit, t.baseline_tarball_url, &set->baseline}}) {
        build::BuildOptions b;
        b.work_dir = cfg_.work_dir;
        b.tarball_url = url;
        b.cache_key = sha + "-" + arch_target_;  // §8.2: cache por (commit, arch)
        b.make_args = cfg_.make_args;
        b.cancel = stop_;
        std::string log;
        const auto bin = build::build_engine(b, log);
        if (!bin) {
            if (!*stop_) {
                log_error("falha ao compilar " + sha + " — este client não jogará o teste " +
                          std::to_string(t.test_id) + ":\n" + log);
            }
            return false;
        }
        *dest = *bin;
    }

    const EffectiveTc tc = effective_tc(*set, cpu_factor_);
    std::string adj;
    for (const auto& a : adjudication_args(set->adjudication)) adj += " " + a;
    log_info("engines prontos; TC " + fastchess_tc(tc) + ", hash " +
             std::to_string(set->hash_mb) + " MB, adjudicação:" +
             (adj.empty() ? std::string(" desligada") : adj));
    std::lock_guard lock(mutex_);
    engines_ = std::move(set);
    return true;
}

void Runner::slot_loop(SlotInfo slot) {
    const std::string tag =
        "slot " + std::to_string(slot.index) +
        (slot.core ? " (núcleo " + std::to_string(*slot.core) + ")" : std::string());
    auto backoff = 2s;

    while (!*stop_) {
        const auto e = engines();
        if (!e) {
            sleep_for(2s);
            continue;
        }

        ClaimResult c;
        try {
            c = api_.claim(client_id_, 1);
            backoff = 2s;
        } catch (const TransientError& ex) {
            log_warn(tag + ": claim falhou (" + ex.what() + "); nova tentativa em " +
                     std::to_string(backoff.count()) + "s");
            sleep_for(backoff);
            backoff = std::min(backoff * 2, std::chrono::seconds(60));
            continue;
        } catch (const std::exception& ex) {
            log_error(tag + ": claim: " + ex.what());
            sleep_for(cfg_.poll_interval);
            continue;
        }

        if (c.status == ClaimStatus::Banned) {
            log_error("server recusou o claim: client banido; encerrando");
            banned_ = true;
            request_stop();
            break;
        }
        if (c.status == ClaimStatus::NoActiveTest || c.pairs.empty()) {
            sleep_for(cfg_.poll_interval);
            continue;
        }

        const auto& pair = c.pairs.front();
        if (c.test_id != e->test_id) {
            // Teste mudou entre polls: devolve e espera os engines novos.
            try {
                api_.abandon(pair.pair_id, client_id_);
            } catch (const std::exception&) {
            }
            wake();
            sleep_for(2s);
            continue;
        }

        const auto body = play_pair(slot, *e, pair);
        if (!body) {  // interrompido pelo shutdown: devolve o lease (§8.5)
            try {
                api_.abandon(pair.pair_id, client_id_);
                log_info(tag + ": par " + std::to_string(pair.pair_id) + " devolvido");
            } catch (const std::exception& ex) {
                log_warn(tag + ": não foi possível devolver o par " +
                         std::to_string(pair.pair_id) + " (" + ex.what() +
                         "); ele volta à fila quando o lease expirar");
            }
            break;
        }
        submit_with_retry(pair.pair_id, *body);

        const int done = ++pairs_done_;
        if (cfg_.max_pairs > 0 && done >= cfg_.max_pairs) {
            log_info("--max-pairs " + std::to_string(cfg_.max_pairs) + " atingido; encerrando");
            request_stop();
        }
    }
}

std::optional<json> Runner::play_pair(const SlotInfo& slot, const EngineSet& e,
                                      const ClaimedPair& pair) {
    const fs::path dir = fs::path(cfg_.work_dir) / "slots" / std::to_string(slot.index);
    fs::create_directories(dir);
    const fs::path epd = dir / "pair.epd";
    const fs::path pgn = dir / "pair.pgn";
    fs::remove(pgn);
    {
        std::ofstream out(epd);
        out << pair.fen << "\n";
    }

    const EffectiveTc tc = effective_tc(e, cpu_factor_);
    proc::ProcessOptions o;
    o.cwd = dir.string();
    o.cancel = stop_;
    if (slot.core) o.cpu = *slot.core;
    // Folga ampla: 2 partidas × 2 lados × (base + 200 incrementos), + 2 min.
    o.timeout = std::chrono::milliseconds(4LL * (tc.base_ms + 200LL * tc.increment_ms) + 120'000);

    const auto start = std::chrono::steady_clock::now();
    const auto r = proc::run_process(
        fastchess_args(cfg_.fastchess, e, tc, epd, pgn,
                       "capi_net test " + std::to_string(e.test_id) + " pair " +
                           std::to_string(pair.pair_id)),
        o);
    if (r.cancelled) return std::nullopt;

    std::string pgn_text;
    {
        std::ifstream in(pgn);
        std::stringstream ss;
        ss << in.rdbuf();
        pgn_text = ss.str();
    }
    std::string note;
    if (r.timed_out) note = "fastchess excedeu o tempo limite e foi encerrado.";
    else if (r.exit_code != 0) note = "fastchess saiu com código " + std::to_string(r.exit_code) + ".";
    if (!note.empty()) {
        note += " Saída: " + proc::tail_lines(r.output, 10);
        log_warn("slot " + std::to_string(slot.index) + ": " + note);
    }

    auto body = make_result_body(client_id_, pgn_text, note, slot, tc, cpu_factor_);
    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
    std::string summary;
    for (const auto& g : body["games"]) {
        summary += (summary.empty() ? "" : ", ") + g["outcome"].get<std::string>() + "/" +
                   g["termination"].get<std::string>();
    }
    log_info("slot " + std::to_string(slot.index) + ": par " + std::to_string(pair.pair_id) +
             " (posição " + std::to_string(pair.position_seq) + ") em " +
             std::to_string(secs.count()) + "s: " + summary);
    return body;
}

void Runner::submit_with_retry(std::int64_t pair_id, const json& body) {
    auto backoff = 2s;
    const auto deadline = std::chrono::steady_clock::now() + 15min;
    int attempts_after_stop = 0;
    while (true) {
        try {
            switch (api_.submit(pair_id, body)) {
                case SubmitStatus::Accepted:
                case SubmitStatus::AlreadyCompleted: return;
                case SubmitStatus::LeaseLost:
                    log_warn("par " + std::to_string(pair_id) +
                             ": lease perdido (expirou); resultado descartado");
                    return;
                case SubmitStatus::TestFinished:
                    log_info("par " + std::to_string(pair_id) +
                             ": teste já terminou; resultado descartado");
                    wake();
                    return;
                case SubmitStatus::ClientBanned:
                    log_error("server informou que este client foi banido; encerrando");
                    banned_ = true;
                    request_stop();
                    return;
            }
        } catch (const TransientError& e) {
            if (std::chrono::steady_clock::now() > deadline ||
                (*stop_ && ++attempts_after_stop > 3)) {
                log_error("par " + std::to_string(pair_id) + ": desistindo do envio (" + e.what() +
                          "); o lease vai expirar e o par volta à fila");
                return;
            }
            log_warn("par " + std::to_string(pair_id) + ": envio falhou (" + e.what() +
                     "); nova tentativa em " + std::to_string(backoff.count()) + "s");
            std::this_thread::sleep_for(backoff);  // não interrompível: queremos entregar
            backoff = std::min(backoff * 2, std::chrono::seconds(60));
        } catch (const std::exception& e) {
            // 4xx inesperado: bug de protocolo; reenviar não ajuda.
            log_error("par " + std::to_string(pair_id) + ": resultado rejeitado: " + e.what());
            return;
        }
    }
}

}  // namespace capi::client
