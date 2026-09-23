#include "client/api.hpp"

namespace capi::client {

using nlohmann::json;

Api::Api(std::string base_url) : base_url_(std::move(base_url)) {}

Api::Response Api::request(const std::string& method, const std::string& path,
                           const std::optional<json>& body) {
    httplib::Client cli(base_url_);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(60);
    const std::string payload = body ? body->dump() : "";
    const auto r = method == "GET" ? cli.Get(path)
                                   : cli.Post(path, payload, "application/json");
    if (!r) {
        throw TransientError(method + " " + path + ": " + httplib::to_string(r.error()));
    }
    json j = r->body.empty() ? json() : json::parse(r->body, nullptr, false);
    if (j.is_discarded()) j = json();
    if (r->status >= 500) {
        throw TransientError(method + " " + path + ": HTTP " + std::to_string(r->status));
    }
    return {r->status, std::move(j)};
}

namespace {
std::string describe(int status, const json& body) {
    std::string s = "HTTP " + std::to_string(status);
    if (body.is_object() && body.contains("error")) s += " " + body["error"].dump();
    if (body.is_object() && body.contains("reason")) s += " " + body["reason"].dump();
    return s;
}
}  // namespace

std::int64_t Api::register_client(const json& info) {
    const auto r = request("POST", "/api/clients/register", info);
    if (r.status == 403) throw ProtocolError("client banido pelo server");
    if (r.status != 200) throw ProtocolError("register: " + describe(r.status, r.body));
    return r.body.at("client_id").get<std::int64_t>();
}

bool Api::heartbeat(std::int64_t client_id, double cpu_factor) {
    const auto r = request("POST", "/api/clients/" + std::to_string(client_id) + "/heartbeat",
                           json{{"cpu_factor", cpu_factor}});
    if (r.status != 200) throw ProtocolError("heartbeat: " + describe(r.status, r.body));
    return r.body.at("banned").get<bool>();
}

std::optional<ActiveTest> Api::active_test() {
    const auto r = request("GET", "/api/active_test", std::nullopt);
    if (r.status == 204) return std::nullopt;
    if (r.status != 200) throw ProtocolError("active_test: " + describe(r.status, r.body));
    const auto& b = r.body;
    ActiveTest t{b.at("test_id").get<std::int64_t>(),
                      b.at("kind").get<std::string>(),
                      b.at("candidate_commit").get<std::string>(),
                      b.at("baseline_commit").get<std::string>(),
                      b.at("candidate_tarball_url").get<std::string>(),
                      b.at("baseline_tarball_url").get<std::string>(),
                      b.at("tc_base_ms").get<int>(),
                      b.at("tc_increment_ms").get<int>(),
                      b.at("hash_mb").get<int>(),
                      b.at("threads").get<int>(),
                      {}};
    if (b.contains("adjudication")) {
        const auto& adj = b["adjudication"];
        if (adj.contains("draw") && adj["draw"].is_object()) {
            const auto& d = adj["draw"];
            t.adjudication.draw = DrawAdjudication{d.at("movenumber").get<int>(),
                                                   d.at("movecount").get<int>(),
                                                   d.at("score_cp").get<int>()};
        }
        if (adj.contains("resign") && adj["resign"].is_object()) {
            const auto& r2 = adj["resign"];
            t.adjudication.resign = ResignAdjudication{r2.at("movecount").get<int>(),
                                                       r2.at("score_cp").get<int>(),
                                                       r2.at("twosided").get<bool>()};
        }
    }
    return t;
}

ClaimResult Api::claim(std::int64_t client_id, int slots_free) {
    const auto r = request("POST", "/api/jobs/claim",
                           json{{"client_id", client_id}, {"slots_free", slots_free}});
    ClaimResult out;
    if (r.status == 204) {
        out.status = ClaimStatus::NoActiveTest;
        return out;
    }
    if (r.status == 403) {
        out.status = ClaimStatus::Banned;
        return out;
    }
    if (r.status != 200) throw ProtocolError("claim: " + describe(r.status, r.body));
    out.test_id = r.body.at("test_id").get<std::int64_t>();
    for (const auto& p : r.body.at("pairs")) {
        out.pairs.push_back({p.at("pair_id").get<std::int64_t>(), p.at("position_seq").get<int>(),
                             p.at("fen").get<std::string>()});
    }
    return out;
}

SubmitStatus Api::submit(std::int64_t pair_id, const json& body) {
    const auto r = request("POST", "/api/jobs/" + std::to_string(pair_id) + "/result", body);
    if (r.status == 200) return SubmitStatus::Accepted;
    if (r.status == 409 && r.body.is_object()) {
        const auto reason = r.body.value("reason", "");
        if (reason == "already_completed") return SubmitStatus::AlreadyCompleted;
        if (reason == "lease_lost") return SubmitStatus::LeaseLost;
        if (reason == "test_finished") return SubmitStatus::TestFinished;
        if (reason == "client_banned") return SubmitStatus::ClientBanned;
    }
    throw ProtocolError("result: " + describe(r.status, r.body));
}

void Api::abandon(std::int64_t pair_id, std::int64_t client_id) {
    // 409 = o lease já não era nosso; nada a fazer.
    request("POST", "/api/jobs/" + std::to_string(pair_id) + "/abandon",
            json{{"client_id", client_id}});
}

}  // namespace capi::client
