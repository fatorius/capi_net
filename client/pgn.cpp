#include "client/pgn.hpp"

#include <cmath>
#include <cstdint>
#include <regex>
#include <sstream>

namespace capi::client {

std::vector<std::string> split_pgn_games(const std::string& pgn) {
    // Cada partida começa numa linha "[Event ".
    std::vector<std::string> games;
    std::size_t pos = pgn.find("[Event ");
    while (pos != std::string::npos) {
        const std::size_t next = pgn.find("\n[Event ", pos + 1);
        const std::size_t end = next == std::string::npos ? pgn.size() : next + 1;
        std::string g = pgn.substr(pos, end - pos);
        while (!g.empty() && (g.back() == '\n' || g.back() == '\r')) g.pop_back();
        games.push_back(g + "\n");
        pos = next == std::string::npos ? std::string::npos : next + 1;
    }
    return games;
}

namespace {

std::map<std::string, std::string> tags_of(const std::string& game) {
    static const std::regex tag_re(R"re(^\[(\w+)\s+"((?:[^"\\]|\\.)*)"\]\s*$)re");
    std::map<std::string, std::string> tags;
    std::istringstream in(game);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, tag_re)) tags[m[1].str()] = m[2].str();
        else if (!line.empty() && line.front() != '[') break;  // início dos lances
    }
    return tags;
}

}  // namespace

std::vector<std::map<std::string, std::string>> pgn_tags(const std::string& pgn) {
    std::vector<std::map<std::string, std::string>> out;
    for (const auto& g : split_pgn_games(pgn)) out.push_back(tags_of(g));
    return out;
}

std::string map_termination(const std::string& tag) {
    if (tag.empty() || tag == "normal") return "normal";
    if (tag == "time forfeit") return "timeout";
    if (tag == "illegal move" || tag == "rules infraction") return "illegal_move";
    if (tag == "adjudication") return "adjudication";
    if (tag == "abandoned" || tag == "stalled connection" || tag == "crash" ||
        tag == "disconnect") {
        return "crash";
    }
    return "other";
}

std::optional<int> parse_duration(const std::string& s) {
    static const std::regex re(R"((\d+):(\d{2}):(\d{2}))");
    std::smatch m;
    if (!std::regex_match(s, m, re)) return std::nullopt;
    const long long ms =
        (std::stoll(m[1]) * 3600 + std::stoll(m[2]) * 60 + std::stoll(m[3])) * 1000LL;
    if (ms > INT32_MAX) return std::nullopt;
    return static_cast<int>(ms);
}

ColorUsage usage_by_color(const std::string& game_pgn, bool white_first) {
    // Lances ficam depois da linha em branco que encerra as tags.
    const auto body_start = game_pgn.find("\n\n");
    if (body_start == std::string::npos) return {};
    static const std::regex time_re(R"(\s(\d+(?:\.\d+)?)s\b)");
    static const std::regex nodes_re(R"(\bn=(\d+))");

    EngineUsage side[2];
    bool seen[2] = {false, false};
    bool white_to_move = white_first;
    std::size_t pos = body_start;
    while ((pos = game_pgn.find('{', pos)) != std::string::npos) {
        const auto end = game_pgn.find('}', pos);
        if (end == std::string::npos) break;
        const std::string comment = game_pgn.substr(pos, end - pos);
        pos = end + 1;

        std::smatch t, n;
        const int i = white_to_move ? 0 : 1;
        white_to_move = !white_to_move;  // um comentário por lance
        if (!std::regex_search(comment, n, nodes_re) || !std::regex_search(comment, t, time_re)) {
            continue;
        }
        side[i].nodes += std::stoll(n[1].str());
        side[i].time_ms += std::llround(std::stod(t[1].str()) * 1000.0);
        seen[i] = true;
    }
    ColorUsage out;
    if (seen[0]) out.white = side[0];
    if (seen[1]) out.black = side[1];
    return out;
}

std::optional<ParsedGame> parse_game(const std::string& game_pgn,
                                     const std::string& candidate_name) {
    ParsedGame g;
    g.text = game_pgn;
    g.tags = tags_of(game_pgn);

    const auto white = g.tags["White"], black = g.tags["Black"];
    if (white == candidate_name) g.candidate_is_white = true;
    else if (black == candidate_name) g.candidate_is_white = false;
    else return std::nullopt;

    const auto& result = g.tags["Result"];
    if (result == "1/2-1/2") {
        g.outcome = "draw";
    } else if (result == "1-0" || result == "0-1") {
        const bool white_won = result == "1-0";
        g.outcome = white_won == g.candidate_is_white ? "candidate_win" : "candidate_loss";
    } else {
        return std::nullopt;
    }

    g.termination = map_termination(g.tags.contains("Termination") ? g.tags["Termination"] : "");
    if (g.tags.contains("PlyCount")) {
        try {
            g.ply_count = std::stoi(g.tags["PlyCount"]);
        } catch (const std::exception&) {
        }
    }
    if (g.tags.contains("GameDuration")) g.duration_ms = parse_duration(g.tags["GameDuration"]);

    // Quem abre: o lado a mover na FEN inicial (2º campo); sem FEN, brancas.
    bool white_first = true;
    if (g.tags.contains("FEN")) {
        const auto& fen = g.tags["FEN"];
        const auto sp = fen.find(' ');
        white_first = !(sp != std::string::npos && sp + 1 < fen.size() && fen[sp + 1] == 'b');
    }
    const auto usage = usage_by_color(game_pgn, white_first);
    g.candidate_usage = g.candidate_is_white ? usage.white : usage.black;
    g.baseline_usage = g.candidate_is_white ? usage.black : usage.white;
    return g;
}

}  // namespace capi::client
