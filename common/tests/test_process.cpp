#include "common/process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace capi::proc;
using namespace std::chrono_literals;

TEST_CASE("captures stdout, stderr and exit code") {
    const auto r = run_process({"sh", "-c", "echo out; echo err 1>&2; exit 3"});
    CHECK(r.exit_code == 3);
    CHECK_FALSE(r.timed_out);
    CHECK(r.output.find("out\n") != std::string::npos);
    CHECK(r.output.find("err\n") != std::string::npos);
}

TEST_CASE("arguments are passed verbatim, without a shell") {
    const auto r = run_process({"printf", "%s|", "a b", "$(whoami)", ";rm"});
    CHECK(r.exit_code == 0);
    CHECK(r.output == "a b|$(whoami)|;rm|");
}

TEST_CASE("feeds stdin and closes it") {
    ProcessOptions o;
    o.stdin_data = "uci\nisready\nquit\n";
    const auto r = run_process({"cat"}, o);
    CHECK(r.exit_code == 0);
    CHECK(r.output == o.stdin_data);
}

TEST_CASE("large stdin does not deadlock against large stdout") {
    ProcessOptions o;
    o.stdin_data.assign(1 << 20, 'x');
    o.max_output = 2 << 20;
    o.timeout = 10s;
    const auto r = run_process({"cat"}, o);
    CHECK_FALSE(r.timed_out);
    CHECK(r.output.size() == o.stdin_data.size());
}

TEST_CASE("timeout kills the whole process group") {
    ProcessOptions o;
    o.timeout = 300ms;
    const auto start = std::chrono::steady_clock::now();
    const auto r = run_process({"sh", "-c", "sleep 5 & sleep 5; echo never"}, o);
    CHECK(r.timed_out);
    CHECK(std::chrono::steady_clock::now() - start < 3s);
    CHECK(r.output.find("never") == std::string::npos);
}

TEST_CASE("cancel flag stops a running process") {
    std::atomic<bool> cancel{false};
    ProcessOptions o;
    o.cancel = &cancel;
    std::thread t([&] {
        std::this_thread::sleep_for(200ms);
        cancel = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const auto r = run_process({"sleep", "5"}, o);
    t.join();
    CHECK(r.cancelled);
    CHECK_FALSE(r.timed_out);
    CHECK(std::chrono::steady_clock::now() - start < 2s);
}

#if defined(__linux__)
TEST_CASE("child is pinned to the requested CPU") {
    ProcessOptions o;
    o.cpu = 0;
    const auto r = run_process({"grep", "Cpus_allowed_list", "/proc/self/status"}, o);
    CHECK(r.exit_code == 0);
    CHECK(r.output.find("\t0\n") != std::string::npos);
}
#else
TEST_CASE("CPU affinity is rejected outside Linux") {
    ProcessOptions o;
    o.cpu = 0;
    CHECK_THROWS_AS(run_process({"true"}, o), std::invalid_argument);
}
#endif

TEST_CASE("runs in the requested directory") {
    ProcessOptions o;
    o.cwd = "/";
    CHECK(run_process({"pwd"}, o).output == "/\n");
}

TEST_CASE("missing program exits 127") {
    CHECK(run_process({"capi-net-no-such-program"}).exit_code == 127);
}

TEST_CASE("output is capped to the last bytes") {
    ProcessOptions o;
    o.max_output = 10;
    const auto r = run_process({"sh", "-c", "i=0; while [ $i -lt 1000 ]; do echo line$i; i=$((i+1)); done"}, o);
    CHECK(r.output.size() == 10);
    CHECK(r.output.ends_with("line999\n"));
}

TEST_CASE("tail_lines keeps the last lines") {
    CHECK(tail_lines("a\nb\nc\nd\n", 2) == "c\nd");
    CHECK(tail_lines("a\nb", 5) == "a\nb");
    CHECK(tail_lines("", 3).empty());
}
