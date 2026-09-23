#include "server/github.hpp"

#include "common/process.hpp"

#include <nlohmann/json.hpp>

#include <chrono>

namespace capi::server {

bool is_full_sha(const std::string& s) {
    if (s.size() != 40) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string tarball_url(const ServerConfig& cfg, const std::string& sha) {
    return cfg.github_codeload + "/" + cfg.github_repo + "/tar.gz/" + sha;
}

std::string resolve_ref(const ServerConfig& cfg, const std::string& ref) {
    // Config do curl via stdin (-K -): o token não aparece na linha de comando.
    std::string curl_cfg;
    curl_cfg += "url = \"" + cfg.github_api + "/repos/" + cfg.github_repo + "/commits/" + ref +
                "\"\n";
    curl_cfg += "header = \"Accept: application/vnd.github+json\"\n";
    curl_cfg += "header = \"X-GitHub-Api-Version: 2022-11-28\"\n";
    curl_cfg += "header = \"User-Agent: capi_net\"\n";
    if (!cfg.github_token.empty()) {
        curl_cfg += "header = \"Authorization: Bearer " + cfg.github_token + "\"\n";
    }
    curl_cfg += "silent\nshow-error\nlocation\nmax-time = 20\n";
    curl_cfg += "write-out = \"\\n%{http_code}\"\n";

    proc::ProcessOptions opts;
    opts.stdin_data = curl_cfg;
    opts.timeout = std::chrono::seconds(30);
    const auto r = proc::run_process({"curl", "-K", "-"}, opts);
    if (r.exit_code != 0) {
        throw GithubError("GitHub API unreachable: " + proc::tail_lines(r.output, 3), false);
    }

    const auto nl = r.output.rfind('\n');
    const std::string code = nl == std::string::npos ? "" : r.output.substr(nl + 1);
    const std::string body = nl == std::string::npos ? r.output : r.output.substr(0, nl);

    if (code == "404" || code == "422") {
        throw GithubError("ref not found in " + cfg.github_repo + ": " + ref, true);
    }
    if (code != "200") {
        throw GithubError("GitHub API returned HTTP " + code + " for ref " + ref, false);
    }

    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("sha") || !j["sha"].is_string() ||
        !is_full_sha(j["sha"].get<std::string>())) {
        throw GithubError("unexpected GitHub API response for ref " + ref, false);
    }
    return j["sha"].get<std::string>();
}

}  // namespace capi::server
