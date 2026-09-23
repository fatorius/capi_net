# capi_net
distributed SPRT testing system for capizero

## Build e testes

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Banco

```bash
createdb capi_net
bash scripts/migrate.sh capi_net   # aplica as migrations pendentes; idempotente
```

## Books

```bash
bash scripts/download_uho.sh   # baixa UHO_Lichess_4852_v1.epd para books/
```

## Server

```bash
CAPI_DB_URL="dbname=capi_net" CAPI_HTTP_PORT=8080 ./build/capi_net_server
```

Dependências (Ubuntu): `build-essential cmake curl postgresql libpq-dev zlib1g-dev`.

Como serviço (inicia no boot e reinicia se cair): ver as instruções no topo de
`scripts/systemd/capi_net_server.service`. Logs: `journalctl --user -u capi_net_server -f`.

Variáveis (ver `server/config.hpp`):

| Variável | Padrão | |
|---|---|---|
| `CAPI_DB_URL` | `dbname=capi_net` | conninfo do libpq |
| `CAPI_HTTP_HOST` / `CAPI_HTTP_PORT` | `0.0.0.0` / `8080` | |
| `CAPI_DB_POOL_SIZE` | `8` | conexões ao Postgres |
| `CAPI_LEASE_TTL_MIN_S` | `600` | lease mínimo de um par |
| `CAPI_MAX_ATTEMPTS` | `5` | tentativas antes de descartar um par |
| `CAPI_MAINTENANCE_INTERVAL_S` | `30` | recuperação de leases e promoção |
| `CAPI_BENCH_REFERENCE_COMMIT` | — | commit de calibração repassado aos clients |
| `CAPI_GITHUB_REPO` | `fatorius/capizero` | |
| `CAPI_GITHUB_TOKEN` | — | opcional; sobe o rate limit da API (60/h sem token) |
| `CAPI_BOOKS_DIR` | `books` | |
| `CAPI_WORK_DIR` | `work` | downloads e builds do smoke test |
| `CAPI_SMOKE_BENCH_DEPTH` | `6` | |
| `CAPI_BUILD_MAKE_ARGS` | — | argumentos extras do `make build` (ex: `COMP=clang`); `PEXT=false` é automático sem BMI2 |

## CLI

```bash
export CAPI_SERVER=http://localhost:8080
capi_net submit --candidate feature/lmr --baseline v2.1.0 --tc 10+0.1 \
    --preset gainer --book UHO_Lichess_4852_v1.epd --pairs 15000 --name "LMR tweak" --wait
capi_net list
capi_net status <id>
capi_net validation <id>
capi_net stop <id>
capi_net priority <id> <n>
```

## Client

Linux ou macOS. Hosts com núcleos heterogêneos (P/E) precisam de afinidade de
CPU; no Apple Silicon o client roda no modo degradado `qos_hint`, com no máximo
P-cores − 2 slots (plano §3.3/§12). Não precisa de libpq nem zlib:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCAPI_NET_BUILD_SERVER=OFF
cmake --build build -j
./build/capi_net_client --check      # mostra topologia, slots padrão e admissão
```

O client não roda como serviço: inicie-o quando a máquina estiver disponível e
encerre com Ctrl+C (os pares em curso são devolvidos ao server).

```bash
./build/capi_net_client --server http://<host-do-server>:8080 --slots <N>
```

- `--slots` é o número de partidas simultâneas; o padrão é `nproc/4`. Use no
  máximo um slot por núcleo físico rápido (em `qos_hint`, P-cores − 2); o
  `--check` informa o limite deste host.
- Rodando o client numa máquina remota via SSH, use `ssh -t` para que o Ctrl+C
  chegue ao client; sem terminal, ele não devolve os pares e eles só voltam à
  fila quando o lease expira:

  ```bash
  ssh -t <usuario>@<host> '<caminho>/build/capi_net_client --server http://<host-do-server>:8080 --slots <N>'
  ```

Na primeira execução o client baixa e compila o fastchess na versão fixa do
capi_net (`kFastchessTag` em `client/fastchess_setup.hpp`) em
`~/.cache/capi_net/tools/`; a versão é reportada ao server. `--fastchess CAMINHO`
usa outro binário. As opções também podem ficar em
`~/.config/capi_net/client.conf` (ver `client/config.hpp`); as flags têm
precedência. Requer `make`, `g++`/`clang++`, `curl` e `tar`.

## Web UI

O server serve a interface em `http://<server>:8080/`: fila e histórico em
`/tests`, detalhe com LLR × pares, pentanomial, Elo e clients em `/tests/<id>`,
e o formulário de criação em `/tests/new` (público: v1 é ferramenta interna).
Páginas em `web/` (HTML/JS sem build; `CAPI_WEB_DIR`).

## Testes

```bash
ctest --test-dir build --output-on-failure
python3 server/tests/integration_test.py build/capi_net_server   # Postgres local; GitHub simulado
```
