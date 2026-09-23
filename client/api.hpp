#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace capi::client {

// Falha de transporte ou resposta 5xx: vale tentar de novo.
struct TransientError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Resposta 4xx inesperada: tentar de novo não resolve.
struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Regras de adjudicação do teste (congeladas no server). nullopt = desligada.
struct DrawAdjudication {
    int movenumber, movecount, score_cp;
};
struct ResignAdjudication {
    int movecount, score_cp;
    bool twosided;
};
struct Adjudication {
    std::optional<DrawAdjudication> draw;
    std::optional<ResignAdjudication> resign;
};

struct ActiveTest {
    std::int64_t test_id;
    std::string kind;
    std::string candidate_commit, baseline_commit;
    std::string candidate_tarball_url, baseline_tarball_url;
    int tc_base_ms, tc_increment_ms, hash_mb, threads;
    Adjudication adjudication;
};

struct ClaimedPair {
    std::int64_t pair_id;
    int position_seq;
    std::string fen;
};

enum class ClaimStatus { Ok, NoActiveTest, Banned };
struct ClaimResult {
    ClaimStatus status = ClaimStatus::Ok;
    std::int64_t test_id = 0;
    std::vector<ClaimedPair> pairs;
};

// Respostas de POST /api/jobs/{id}/result (plano §5.5).
enum class SubmitStatus {
    Accepted,
    AlreadyCompleted,  // retry de envio já aceito: sucesso
    LeaseLost,         // descartar
    TestFinished,      // descartar
    ClientBanned,      // parar
};

// Cliente da API do server (§9). Thread-safe: uma conexão HTTP por chamada.
class Api {
public:
    explicit Api(std::string base_url);

    std::int64_t register_client(const nlohmann::json& info);  // lança ProtocolError se banido
    bool heartbeat(std::int64_t client_id, double cpu_factor);  // true = banido
    std::optional<ActiveTest> active_test();
    ClaimResult claim(std::int64_t client_id, int slots_free);
    SubmitStatus submit(std::int64_t pair_id, const nlohmann::json& body);
    void abandon(std::int64_t pair_id, std::int64_t client_id);

private:
    struct Response {
        int status;
        nlohmann::json body;
    };
    Response request(const std::string& method, const std::string& path,
                     const std::optional<nlohmann::json>& body);

    std::string base_url_;
};

}  // namespace capi::client
