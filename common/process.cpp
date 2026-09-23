#include "common/process.hpp"

#if defined(__linux__)
#include <sched.h>
#endif
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace capi::proc {

namespace {

void set_flag(int fd, int cmd_get, int cmd_set, int flag) {
    const int flags = fcntl(fd, cmd_get);
    if (flags >= 0) fcntl(fd, cmd_set, flags | flag);
}

void make_pipe(int fds[2]) {
    if (pipe(fds) != 0) throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
    // As pontas do pai não podem vazar para outros filhos criados em paralelo.
    set_flag(fds[0], F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(fds[1], F_GETFD, F_SETFD, FD_CLOEXEC);
}

}  // namespace

ProcessResult run_process(const std::vector<std::string>& argv, const ProcessOptions& opts) {
    if (argv.empty()) throw std::invalid_argument("run_process: empty argv");

    // Um filho que sai sem ler todo o stdin faria o write() gerar SIGPIPE.
    static std::once_flag ignore_sigpipe;
    std::call_once(ignore_sigpipe, [] { signal(SIGPIPE, SIG_IGN); });

#if !defined(__linux__)
    if (opts.cpu >= 0) throw std::invalid_argument("run_process: CPU affinity requires Linux");
#else
    cpu_set_t cpu_mask;
    CPU_ZERO(&cpu_mask);
    if (opts.cpu >= 0) CPU_SET(opts.cpu, &cpu_mask);
#endif

    // Tudo que o filho usa é preparado antes do fork.
    std::vector<char*> cargv;
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    const char* cwd = opts.cwd.empty() ? nullptr : opts.cwd.c_str();

    int in_pipe[2], out_pipe[2];
    make_pipe(in_pipe);
    make_pipe(out_pipe);

    const pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) close(fd);
        throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    }

    if (pid == 0) {
        // Filho: apenas chamadas async-signal-safe até o exec.
        setpgid(0, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        signal(SIGPIPE, SIG_DFL);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        if (cwd && chdir(cwd) != 0) _exit(127);
#if defined(__linux__)
        if (opts.cpu >= 0 && sched_setaffinity(0, sizeof(cpu_mask), &cpu_mask) != 0) _exit(126);
#endif
        execvp(cargv[0], cargv.data());
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    int in_fd = in_pipe[1];
    const int out_fd = out_pipe[0];
    set_flag(in_fd, F_GETFL, F_SETFL, O_NONBLOCK);

    std::size_t written = 0;
    if (opts.stdin_data.empty()) {
        close(in_fd);
        in_fd = -1;
    }

    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + opts.timeout;
    bool out_open = true;
    char buf[8192];

    while (out_open) {
        if (opts.cancel && opts.cancel->load()) {
            result.cancelled = true;
            kill(-pid, SIGKILL);
            break;
        }
        int wait_ms = -1;
        if (opts.timeout.count() > 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                result.timed_out = true;
                kill(-pid, SIGKILL);
                break;
            }
            wait_ms = static_cast<int>(left.count());
        }
        if (opts.cancel && (wait_ms < 0 || wait_ms > 200)) wait_ms = 200;

        pollfd fds[2];
        nfds_t n = 0;
        fds[n++] = {out_fd, POLLIN, 0};
        if (in_fd >= 0) fds[n++] = {in_fd, POLLOUT, 0};

        const int rc = poll(fds, n, wait_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            kill(-pid, SIGKILL);
            break;
        }

        if (n == 2 && fds[1].revents) {
            if (fds[1].revents & POLLOUT) {
                const ssize_t w = write(in_fd, opts.stdin_data.data() + written,
                                        opts.stdin_data.size() - written);
                if (w > 0) written += static_cast<std::size_t>(w);
                if (w < 0 && errno != EAGAIN && errno != EINTR) written = opts.stdin_data.size();
            } else {
                written = opts.stdin_data.size();  // POLLERR/POLLHUP: filho fechou o stdin
            }
            if (written >= opts.stdin_data.size()) {
                close(in_fd);
                in_fd = -1;
            }
        }

        if (fds[0].revents) {
            const ssize_t r = read(out_fd, buf, sizeof(buf));
            if (r > 0) {
                result.output.append(buf, static_cast<std::size_t>(r));
                if (result.output.size() > 2 * opts.max_output) {
                    result.output.erase(0, result.output.size() - opts.max_output);
                }
            } else if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
                out_open = false;
            }
        }
    }

    if (in_fd >= 0) close(in_fd);
    close(out_fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    // Netos que herdaram o pipe (ex: make → cc) não sobrevivem ao timeout.
    if (result.timed_out || result.cancelled) kill(-pid, SIGKILL);

    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    if (result.output.size() > opts.max_output) {
        result.output.erase(0, result.output.size() - opts.max_output);
    }
    return result;
}

std::string tail_lines(const std::string& text, std::size_t max_lines) {
    std::size_t end = text.size();
    while (end > 0 && text[end - 1] == '\n') --end;
    std::size_t pos = end;
    for (std::size_t lines = 0; pos > 0; --pos) {
        if (text[pos - 1] == '\n' && ++lines == max_lines) break;
    }
    return text.substr(pos, end - pos);
}

}  // namespace capi::proc
