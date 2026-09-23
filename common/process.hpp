#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace capi::proc {

struct ProcessOptions {
    std::string cwd;                        // vazio = diretório atual
    std::string stdin_data;                 // escrito no stdin do filho e então fechado
    std::chrono::milliseconds timeout{0};   // 0 = sem limite
    std::size_t max_output = 1 << 20;       // guarda só os últimos max_output bytes
    const std::atomic<bool>* cancel = nullptr;  // se virar true, mata o processo (≤ 200 ms)
    // Núcleo lógico ao qual o filho (e toda a árvore dele) fica restrito,
    // aplicado entre fork e exec. -1 = sem afinidade. Só Linux.
    int cpu = -1;
};

struct ProcessResult {
    int exit_code = -1;      // 127: programa não executado; 126: afinidade falhou; -1: sinal
    bool timed_out = false;  // morto (com todo o grupo de processos) por timeout
    bool cancelled = false;  // morto por ProcessOptions::cancel
    std::string output;      // stdout + stderr intercalados
};

// Executa argv[0] (busca no PATH) com os argumentos dados, SEM shell: nenhum
// argumento é interpretado. O filho roda num grupo de processos próprio, com
// máscara de sinais e SIGPIPE restaurados. Na primeira chamada, o processo
// chamador passa a ignorar SIGPIPE. POSIX apenas.
ProcessResult run_process(const std::vector<std::string>& argv, const ProcessOptions& opts = {});

// Últimas `max_lines` linhas de `text`, para logs.
std::string tail_lines(const std::string& text, std::size_t max_lines);

}  // namespace capi::proc
