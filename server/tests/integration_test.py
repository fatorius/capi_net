#!/usr/bin/env python3
"""Teste de integração do server: sobe o binário contra um banco descartável e
exercita a API de ponta a ponta (claim, resultado, 409s, leases, promoção,
parada SPRT, esgotamento, concorrência, criação de testes com smoke test e CLI).
A API do GitHub e o codeload são simulados localmente, com uma engine falsa.

Uso:  python3 server/tests/integration_test.py build/capi_net_server

Requer createdb/dropdb/psql no PATH, com as variáveis PG* padrão apontando
para um Postgres local. Só usa a stdlib do Python.
"""

import gzip
import io
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PORT = 18000 + os.getpid() % 1000
BASE = f"http://127.0.0.1:{PORT}"
DB = f"capi_net_it_{os.getpid()}"

failures = 0


def check(cond, msg):
    global failures
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        failures += 1


def psql(sql):
    out = subprocess.run(
        ["psql", "-X", "-q", "-At", "-v", "ON_ERROR_STOP=1", "-d", DB, "-c", sql],
        check=True, capture_output=True, text=True)
    return out.stdout.strip()


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as e:
        raw = e.read()
        return e.code, (json.loads(raw) if raw else None)


def create_test(name, kind, status, priority, pairs, elo0=None, elo1=None,
                tc_base=10000, tc_inc=100):
    cand, base = "a" * 40, "b" * 40
    sprt = f"'custom', {elo0}, {elo1}" if kind == "sprt" else "NULL, NULL, NULL"
    test_id = psql(f"""
        INSERT INTO tests (name, kind, status, candidate_ref, candidate_commit,
                           baseline_ref, baseline_commit, tc_base_ms, tc_increment_ms,
                           book_name, total_pairs, priority, sprt_preset, sprt_elo0, sprt_elo1)
        VALUES ('{name}', '{kind}', '{status}', 'feature/x', '{cand}', 'v1', '{base}',
                {tc_base}, {tc_inc}, 'test.epd', {pairs}, {priority}, {sprt})
        RETURNING id""").splitlines()[0]
    psql(f"""
        INSERT INTO opening_positions (test_id, seq, fen)
        SELECT {test_id}, g, 'fen ' || g FROM generate_series(1, {pairs}) g;
        INSERT INTO job_pairs (test_id, position_id, dispatch_order)
        SELECT {test_id}, id, random() FROM opening_positions WHERE test_id = {test_id};""")
    return int(test_id)


PGN = '[Event "capi_net"]\n[Result "{r}"]\n\n1. e4 e5 2. Nf3 Nc6 {r}\n'


def game(i, outcome, termination="normal", ply=80):
    return {"game_in_pair": i, "candidate_is_white": i == 0, "outcome": outcome,
            "termination": termination, "ply_count": ply, "duration_ms": 1500,
            "cpu_factor": 1.0, "tc_base_effective_ms": 10000,
            "tc_increment_effective_ms": 100, "slot_index": 0, "core_id": 2,
            "pgn": PGN.format(r="1/2-1/2"),
            # candidate a 2 M nós/s, baseline a 1 M nós/s
            "candidate_nodes": 2_000_000, "candidate_time_ms": 1000,
            "baseline_nodes": 1_000_000, "baseline_time_ms": 1000}


def submit(pair_id, client_id, o0, o1, **kw):
    return call("POST", f"/api/jobs/{pair_id}/result",
                {"client_id": client_id, "games": [game(0, o0, **kw), game(1, o1)]})


def register(name, slots, pinning="pinned"):
    return call("POST", "/api/clients/register",
                {"name": name, "hostname": name, "os": "test", "cpu_model": "cpu",
                 "arch_target": "x86-64-avx2", "pinning_mode": pinning,
                 "nproc": 8, "slots": slots, "fastchess_version": "fastchess alpha 1.8.2 test"})


def claim(client_id, n):
    return call("POST", "/api/jobs/claim", {"client_id": client_id, "slots_free": n})


def stats(test_id):
    return call("GET", f"/api/tests/{test_id}/stats")[1]


def wait_for(pred, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------------------
# GitHub falso: API de commits + codeload com tarballs de uma engine de mentira
# ---------------------------------------------------------------------------
SHA_MAIN, SHA_V1, SHA_BROKEN, SHA_OLD = ("1" * 40, "2" * 40, "3" * 40, "4" * 40)
REFS = {"main": SHA_MAIN, "feature/x": SHA_MAIN, SHA_MAIN[:7]: SHA_MAIN, "v1": SHA_V1,
        "broken": SHA_BROKEN, "old": SHA_OLD}

FAKE_ENGINE = """#!/bin/sh
case "$1" in
  bench) echo "position  1/1: depth=$2 nodes=4321 time=1ms"; echo "Nodes: 4321"; echo "NPS: 1000";;
  uci) while read l; do case "$l" in
         uci) echo "id name fake"; echo "uciok";; isready) echo "readyok";; quit) exit 0;;
       esac; done;;
  *) echo "algoritmo iniciado";;
esac
"""


def fake_tarball(sha):
    files = {"engine.sh": FAKE_ENGINE}
    if sha == SHA_BROKEN:
        files["Makefile"] = "build:\n\t@echo 'error: boom in eval.cpp' && exit 1\n"
    elif sha == SHA_OLD:  # Makefile antigo: ignora NAME e o binário não tem modo bench
        files["Makefile"] = "build:\n\tprintf '#!/bin/sh\\necho algoritmo iniciado\\n' > capizero_old && chmod +x capizero_old\n"
    else:
        files["Makefile"] = "build:\n\tcp engine.sh $(NAME) && chmod +x $(NAME)\n"
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, content in files.items():
            data = content.encode()
            info = tarfile.TarInfo(f"capizero-{sha}/{name}")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


class FakeGithub(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        m = re.fullmatch(r"/repos/fatorius/capizero/commits/(.+)", self.path)
        if m:
            ref = m.group(1)
            if ref == "boom500":
                return self._send(500, b'{"message":"server error"}')
            if ref not in REFS:
                return self._send(404, b'{"message":"No commit found"}')
            return self._send(200, json.dumps({"sha": REFS[ref]}).encode())
        m = re.fullmatch(r"/fatorius/capizero/tar.gz/([0-9a-f]{40})", self.path)
        if m:
            return self._send(200, fake_tarball(m.group(1)), "application/gzip")
        self._send(404, b"{}")


def write_book(path, n):
    board = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"
    with open(path, "w") as f:
        f.write("# test book\n\n")
        for i in range(n):
            # metade EPD com opcodes, metade FEN completa
            if i % 2:
                f.write(f"{board} w KQkq - {i} 1\n")
            else:
                f.write(f"{board} b KQkq - bm e5; id \"pos{i}\";\n")


def wait_validation(test_id, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        st, v = call("GET", f"/api/admin/tests/{test_id}/validation")
        if v["status"] != "validating":
            return v
        time.sleep(0.2)
    return v


def cli(cli_bin, *args):
    r = subprocess.run([cli_bin, "--server", BASE, *args], capture_output=True, text=True,
                       timeout=60)
    return r.returncode, r.stdout + r.stderr


def main(server_bin):
    cli_bin = str(Path(server_bin).with_name("capi_net"))
    tmp = Path(tempfile.mkdtemp(prefix="capi_net_it_"))
    (tmp / "books").mkdir()
    write_book(tmp / "books" / "test.epd", 60)
    github = ThreadingHTTPServer(("127.0.0.1", 0), FakeGithub)
    threading.Thread(target=github.serve_forever, daemon=True).start()
    github_url = f"http://127.0.0.1:{github.server_address[1]}"

    subprocess.run(["createdb", DB], check=True)
    server = None
    try:
        subprocess.run(["bash", str(ROOT / "scripts/migrate.sh"), DB], check=True,
                       stdout=subprocess.DEVNULL)

        t1 = create_test("exhaust", "sprt", "queued", 10, 6, 0, 5, tc_base=1, tc_inc=0)
        t2 = create_test("accept", "sprt", "queued", 5, 12, 0, 100)
        t3 = create_test("gauntlet", "gauntlet", "queued", 0, 200)

        env = dict(os.environ, CAPI_DB_URL=f"dbname={DB}", CAPI_HTTP_HOST="127.0.0.1",
                   CAPI_HTTP_PORT=str(PORT), CAPI_DB_POOL_SIZE="4",
                   CAPI_LEASE_TTL_MIN_S="2", CAPI_MAINTENANCE_INTERVAL_S="1",
                   CAPI_BENCH_REFERENCE_COMMIT="c" * 40, CAPI_GITHUB_API=github_url,
                   CAPI_GITHUB_CODELOAD=github_url, CAPI_BOOKS_DIR=str(tmp / "books"),
                   CAPI_WORK_DIR=str(tmp / "work"), CAPI_WEB_DIR=str(ROOT / "web"))
        server = subprocess.Popen([server_bin], env=env)
        check(wait_for(lambda: _healthy()), "server responde /healthz")

        print("promoção")
        check(wait_for(lambda: call("GET", "/api/active_test")[0] == 200),
              "manutenção promove um teste queued")
        st, at = call("GET", "/api/active_test")
        check(at["test_id"] == t1, "maior prioridade vira running")
        check(at["candidate_commit"] == "a" * 40 and "candidate_ref" not in at,
              "active_test expõe só SHAs")
        check(at["bench_reference_commit"] == "c" * 40, "bench_reference_commit repassado")

        print("registro")
        _, r1 = register("c1", 2)
        _, r2 = register("c2", 4)
        c1, c2 = r1["client_id"], r2["client_id"]
        check(register("c1", 2)[1]["client_id"] == c1, "re-registro reaproveita o id")
        check(register("bad", 1, pinning="none")[0] == 400, "pinning_mode inválido -> 400")
        check(call("POST", "/api/clients/999999/heartbeat", {})[0] == 404,
              "heartbeat de client inexistente -> 404")
        st, hb = call("POST", f"/api/clients/{c1}/heartbeat", {"cpu_factor": 1.05})
        check(st == 200 and hb == {"banned": False}, "heartbeat ok")
        check(psql(f"SELECT cpu_factor FROM clients WHERE id={c1}") == "1.05",
              "heartbeat grava cpu_factor")

        print("claim e resultado (teste 1)")
        st, cl = claim(c1, 5)
        pairs = cl["pairs"]
        check(st == 200 and len(pairs) == 2, "claim limitado a clients.slots (2)")
        a, b = pairs[0]["pair_id"], pairs[1]["pair_id"]
        check(pairs[0]["fen"].startswith("fen "), "claim traz a FEN")
        check(claim(999999, 1)[0] == 404, "claim de client inexistente -> 404")

        check(submit(a, c2, "candidate_win", "draw") == (409, {"error": "conflict",
                                                               "reason": "lease_lost"}),
              "resultado de outro client -> 409 lease_lost")
        check(submit(a, c1, "candidate_win", "draw") ==
              (200, {"status": "accepted", "valid_games": 2}), "resultado aceito")
        check(submit(a, c1, "candidate_win", "draw")[1].get("reason") == "already_completed",
              "reenvio -> 409 already_completed")
        check(submit(b, c1, "candidate_loss", "draw", termination="crash")[1]
              == {"status": "accepted", "valid_games": 1}, "crash invalida só a partida")
        check(call("POST", f"/api/jobs/{b}/result",
                   {"client_id": c1, "games": [game(0, "draw")]})[0] == 400,
              "par com 1 partida -> 400")
        bad = game(1, "draw")
        bad["outcome"] = "won"
        check(call("POST", f"/api/jobs/{b}/result",
                   {"client_id": c1, "games": [game(0, "draw"), bad]})[0] == 400,
              "outcome inválido -> 400")

        s = stats(t1)
        check(s["pairs"]["valid"] == 1 and s["penta"]["wd"] == 1,
              "par com crash fora da estatística; W+D conta como WD")
        check(s["pairs"]["completed"] == 2 and s["pairs"]["pending"] == 4, "contagens de pares")

        rows = psql(f"SELECT encode(pgn_gz, 'hex') FROM game_pgns gp JOIN games g "
                    f"ON g.id = gp.game_id WHERE g.pair_id = {a}").splitlines()
        check(len(rows) == 2 and gzip.decompress(bytes.fromhex(rows[0])).decode()
              == PGN.format(r="1/2-1/2"), "PGN gravado com gzip e recuperável")
        check(psql(f"SELECT invalid_reason FROM games WHERE pair_id={b} AND NOT valid")
              == "termination_crash", "invalid_reason gravado")

        print("abandono e expiração de lease")
        pc = claim(c2, 1)[1]["pairs"][0]["pair_id"]
        check(call("POST", f"/api/jobs/{pc}/abandon", {"client_id": c2})[0] == 200,
              "abandon devolve o par")
        check(call("POST", f"/api/jobs/{pc}/abandon", {"client_id": c2})[0] == 409,
              "abandon repetido -> 409")
        pd = claim(c2, 1)[1]["pairs"][0]["pair_id"]
        time.sleep(3.5)
        check(submit(pd, c2, "draw", "draw")[1].get("reason") == "lease_lost",
              "lease expirado -> 409 lease_lost")
        check(stats(t1)["pairs"]["leased"] == 0, "par expirado voltou para pending")

        psql(f"UPDATE job_pairs SET attempts = 5 WHERE test_id = {t1} AND status = 'pending'")
        claim(c1, 2)
        time.sleep(3.5)
        s = stats(t1)
        check(s["pairs"]["discarded"] == 2, "attempts >= max -> discarded (too_many_attempts)")

        print("esgotamento e promoção do próximo")
        rest = claim(c1, 2)[1]["pairs"]
        check(len(rest) == 2, "restam 2 pares")
        for p in rest:
            submit(p["pair_id"], c1, "draw", "draw")
        s = stats(t1)
        check(s["status"] == "finished" and s["result"] == "inconclusive",
              "SPRT sem cruzar fronteira termina inconclusive")
        check(call("GET", "/api/active_test")[1]["test_id"] == t2,
              "próximo teste promovido na mesma transação")

        print("parada SPRT (teste 2, bounds 0/100)")
        held = claim(c2, 1)[1]["pairs"][0]["pair_id"]  # fica em voo durante a parada
        submitted = 0
        while submitted < 10:
            for p in claim(c1, 1)[1]["pairs"]:
                submit(p["pair_id"], c1, "candidate_win", "candidate_win")
                submitted += 1
                s = stats(t2)
                if submitted == 9:
                    check(s["status"] == "running" and
                          abs(s["sprt"]["llr"] - 2.879189755790261) < 1e-6,
                          f"9 WW: LLR {s['sprt']['llr']:.6f} (ref 2.879190), segue")
        s = stats(t2)
        check(s["status"] == "finished" and s["result"] == "accepted" and
              abs(s["sprt"]["llr"] - 3.1991025994376097) < 1e-6,
              f"10 WW: LLR {s['sprt']['llr']:.6f} (ref 3.199103), accepted")
        check(submit(held, c2, "draw", "draw")[1].get("reason") == "test_finished",
              "resultado após parada -> 409 test_finished")

        print("concorrência (teste 3, gauntlet, 8 clients, pool de 4 conexões)")
        check(call("GET", "/api/active_test")[1]["test_id"] == t3, "gauntlet promovido")
        clients = [register(f"w{i}", 4)[1]["client_id"] for i in range(8)]
        lock = threading.Lock()
        seen, tally, errors = [], [0] * 5, []
        score = {"candidate_win": 1.0, "draw": 0.5, "candidate_loss": 0.0}

        def worker(cid):
            rng = random.Random(cid)
            while True:
                st, body = claim(cid, 4)
                if st != 200 or not body["pairs"]:
                    return
                for p in body["pairs"]:
                    o0, o1 = rng.choice(list(score)), rng.choice(list(score))
                    st, res = submit(p["pair_id"], cid, o0, o1)
                    with lock:
                        seen.append(p["pair_id"])
                        if st == 200:
                            tally[int((score[o0] + score[o1]) * 2)] += 1
                        else:
                            errors.append((st, res))

        threads = [threading.Thread(target=worker, args=(c,)) for c in clients]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        s = stats(t3)
        check(not errors, f"nenhum erro nos envios concorrentes {errors[:3]}")
        check(len(seen) == 200 and len(set(seen)) == 200, "cada par entregue exatamente uma vez")
        penta = [s["penta"][k] for k in ("ll", "ld", "dd_wl", "wd", "ww")]
        check(penta == tally, f"penta do server {penta} == contagem local {tally}")
        check(s["status"] == "finished" and s["result"] == "pending",
              "gauntlet termina ao esgotar, sem veredito")

        print("web UI e endpoints de leitura")

        def raw(path):
            with urllib.request.urlopen(BASE + path, timeout=30) as r:
                return r.status, r.headers, r.read().decode()

        for path, marker in [("/tests", "capi_net · testes"), (f"/tests/{t2}", "capi_net · teste"),
                             ("/tests/new", "novo teste"), ("/static/app.js", "function api("),
                             ("/static/app.css", "--accent")]:
            st, _, body = raw(path)
            check(st == 200 and marker in body, f"GET {path}")
        req = urllib.request.Request(BASE + "/")
        opener = urllib.request.build_opener(type("NoRedirect", (urllib.request.HTTPRedirectHandler,),
                                                  {"redirect_request": lambda *a, **k: None}))
        try:
            opener.open(req, timeout=10)
            check(False, "/ redireciona para /tests")
        except urllib.error.HTTPError as e:
            check(e.code in (301, 302) and e.headers["Location"] == "/tests", "/ redireciona para /tests")

        st, cfgj = call("GET", "/api/config")
        check(st == 200 and cfgj["presets"]["gainer"]["elo1"] == 5, "/api/config traz os presets")
        st, books = call("GET", "/api/books")
        check(st == 200 and books == [{"name": "test.epd", "size_bytes": books[0]["size_bytes"],
                                       "positions": 60}], "/api/books lista o book e suas posições")

        st, hist = call("GET", f"/api/tests/{t2}/history")
        check(st == 200 and [p["pairs"] for p in hist] == list(range(1, 11)),
              "histórico do LLR: um ponto por par válido")
        check(abs(hist[-1]["llr"] - 3.1991025994376097) < 1e-6 and
              all(a["llr"] < b["llr"] for a, b in zip(hist, hist[1:])),
              "histórico termina no LLR final e cresce com os WW")
        check(call("GET", "/api/tests/999999/history")[0] == 404, "histórico de teste inexistente -> 404")

        st, cl = call("GET", f"/api/tests/{t3}/clients")
        check(st == 200 and len(cl) == 8 and sum(c["pairs_completed"] for c in cl) == 200 and
              all(c["fastchess_version"] == "fastchess alpha 1.8.2 test" for c in cl),
              "clients do teste: 8, somando 200 pares, com versão do fastchess")

        st, sp = call("GET", f"/api/tests/{t3}/speed")
        check(st == 200 and sp["games"] == 400 and abs(sp["candidate_nps"] - 2e6) < 1 and
              abs(sp["baseline_nps"] - 1e6) < 1 and abs(sp["ratio"] - 2.0) < 1e-9,
              "velocidade do teste: candidate 2 M vs baseline 1 M nós/s (ratio 2.0)")
        check(all(abs(c["nps"] - 1.5e6) < 1 for c in cl), "NPS por client no teste: 1,5 M nós/s")
        st, allc = call("GET", "/api/clients")
        w = [c for c in allc if c["name"].startswith("w")]
        check(st == 200 and len(w) == 8 and all(abs(c["nps"] - 1.5e6) < 1 and c["nps_games"] > 0
                                                 for c in w),
              "/api/clients: NPS recente de cada client")
        st, _, body = raw("/clients")
        check(st == 200 and "capi_net · clients" in body, "GET /clients")
        bad_game = game(0, "draw")
        bad_game["candidate_nodes"] = -5
        check(call("POST", f"/api/jobs/{a}/result",
                   {"client_id": c1, "games": [bad_game, game(1, "draw")]})[0] == 400,
              "contagem de nós negativa -> 400")

        gid = int(psql(f"SELECT min(id) FROM games WHERE test_id = {t3}"))
        st, _, body = raw(f"/api/tests/{t3}/games/{gid}/pgn")
        check(st == 200 and body == PGN.format(r="1/2-1/2"), "PGN de uma partida, descomprimido")
        check(call("GET", f"/api/tests/{t1}/games/{gid}/pgn")[0] == 404,
              "PGN de partida de outro teste -> 404")
        st, headers, body = raw(f"/api/tests/{t3}/pgn")
        check(st == 200 and body.count("[Event ") == 400 and
              "capi_net-test-" in headers.get("Content-Disposition", ""),
              "PGN agregado: 400 partidas em streaming, como anexo")

        print("banimento")
        psql(f"UPDATE clients SET status = 'banned' WHERE id = {c2}")
        check(claim(c2, 1)[0] == 403, "claim de banido -> 403")
        check(call("POST", f"/api/clients/{c2}/heartbeat", {})[1] == {"banned": True},
              "heartbeat informa banimento")
        check(register("c2", 4)[0] == 403, "re-registro não remove banimento")
        check(claim(c1, 1)[0] == 204, "sem teste ativo -> 204")

        print("criação de teste: validação síncrona")
        spec = {"kind": "sprt", "candidate_ref": "feature/x", "baseline_ref": "v1",
                "tc": "10+0.1", "book_name": "test.epd", "total_pairs": 20,
                "preset": "gainer", "name": "A"}

        def create(**over):
            return call("POST", "/api/admin/tests", {**spec, **over})

        for over, err, why in [
            ({"tc": "10/0.1"}, "invalid_spec", "tc inválido"),
            ({"book_name": "missing.epd"}, "invalid_spec", "book inexistente"),
            ({"book_name": "../x.epd"}, "invalid_spec", "book fora do diretório"),
            ({"total_pairs": 61}, "invalid_spec", "mais pares que posições no book"),
            ({"baseline_ref": "nope"}, "ref_not_found", "ref inexistente"),
            ({"baseline_ref": SHA_MAIN[:7]}, "invalid_spec", "refs no mesmo commit"),
            ({"preset": "custom"}, "invalid_spec", "custom sem bounds"),
        ]:
            st, body = create(**over)
            check(st == 400 and body["error"] == err, f"{why} -> 400 {err}")
        st, body = create(baseline_ref="boom500")
        check(st == 502 and body["error"] == "github_unavailable", "GitHub com erro -> 502")
        check(psql("SELECT count(*) FROM tests WHERE name = 'A'") == "0",
              "specs rejeitadas não criam teste")

        print("criação de teste: smoke test e fila")
        st, a_body = create()
        ta = a_body["test_id"]
        check(st == 201 and a_body["status"] == "validating" and
              a_body["candidate"] == {"ref": "feature/x", "commit": SHA_MAIN} and
              a_body["baseline"]["commit"] == SHA_V1, "201 com SHAs resolvidos e congelados")
        v = wait_validation(ta)
        check(v["status"] == "running" and "SMOKE TEST PASSED" in v["validation_log"],
              "smoke passa e o teste é promovido (fila vazia)")
        check("bench ok: Nodes: 4321" in v["validation_log"] and "uciok" in v["validation_log"],
              "log registra bench e handshake UCI")
        fens = psql(f"SELECT fen FROM opening_positions WHERE test_id = {ta} ORDER BY seq").split("\n")
        check(len(fens) == 20 and all(len(f.split()) == 6 for f in fens) and
              any(f.endswith(" b KQkq - 0 1") for f in fens),
              "20 posições amostradas, EPD normalizado para FEN")
        check(psql(f"SELECT count(*) FROM job_pairs WHERE test_id = {ta} AND status = 'pending'")
              == "20", "um par por posição")
        s = stats(ta)
        check(abs(s["sprt"]["llr_upper"] - 2.944438979) < 1e-6 and s["sprt"]["elo1"] == 5,
              "test_stats criado com fronteiras do preset gainer")

        tb = create(name="B", preset="nonreg")[1]["test_id"]
        tc_ = create(name="C", kind="gauntlet", preset=None)[1]["test_id"]
        check(wait_validation(tb)["status"] == "queued" and
              wait_validation(tc_)["status"] == "queued", "com teste rodando, novos vão para a fila")
        check(stats(tb)["sprt"]["elo0"] == -5, "preset nonreg: bounds [-5, 0]")
        check(call("PATCH", f"/api/admin/tests/{tc_}", {"priority": 5})[0] == 200,
              "PATCH priority em teste queued")
        check(call("PATCH", f"/api/admin/tests/{ta}", {"priority": 5})[0] == 409,
              "PATCH priority em teste running -> 409")

        td = create(name="D", candidate_ref="broken")[1]["test_id"]
        v = wait_validation(td)
        check(v["status"] == "invalid" and "boom in eval.cpp" in v["validation_log"],
              "build quebrado -> invalid, com a saída do make no log")
        te = create(name="E", baseline_ref="old")[1]["test_id"]
        v = wait_validation(te)
        check(v["status"] == "invalid" and "using capizero_old" in v["validation_log"] and
              "no positive 'Nodes:'" in v["validation_log"],
              "Makefile antigo: binário achado por fallback; sem bench -> invalid")
        check(psql(f"SELECT count(*) FROM job_pairs WHERE test_id IN ({td}, {te})") == "0",
              "teste inválido não gera jobs")

        st, body = call("POST", f"/api/admin/tests/{ta}/stop")
        check(st == 200 and body["promoted_test_id"] == tc_,
              "stop do running promove o de maior prioridade")
        check(call("POST", f"/api/admin/tests/{ta}/stop")[0] == 409, "stop repetido -> 409")
        order = [t["test_id"] for t in call("GET", "/api/tests")[1]]
        check(order[:2] == [tc_, tb], "lista: running, depois a fila")

        print("CLI")
        rc, out = cli(cli_bin, "submit", "--candidate", "main", "--baseline", "v1", "--tc",
                      "5+0.05", "--book", "test.epd", "--pairs", "10", "--name", "via cli",
                      "--wait")
        check(rc == 0 and f"main → {SHA_MAIN}" in out and f"v1 → {SHA_V1}" in out and
              "queued" in out, "submit imprime os SHAs resolvidos e espera o smoke test")
        cli_id = int(re.search(r"teste (\d+) criado", out).group(1))
        rc, out = cli(cli_bin, "list")
        check(rc == 0 and "via cli" in out, "list")
        rc, out = cli(cli_bin, "status", str(cli_id))
        check(rc == 0 and "LLR" in out and "5+0.05" in out and SHA_MAIN in out, "status")
        rc, out = cli(cli_bin, "priority", str(cli_id), "9")
        check(rc == 0, "priority")
        rc, out = cli(cli_bin, "stop", str(cli_id))
        check(rc == 0 and "parado" in out, "stop")
        rc, out = cli(cli_bin, "stop", str(cli_id))
        check(rc == 1 and "already stopped" in out, "stop repetido falha com mensagem")
        rc, out = cli(cli_bin, "submit", "--candidate", "broken", "--baseline", "v1", "--tc",
                      "5", "--book", "test.epd", "--pairs", "5", "--wait")
        check(rc == 1 and "boom in eval.cpp" in out, "submit --wait de teste inválido sai com 1")

        print("shutdown")
        server.send_signal(signal.SIGTERM)
        check(server.wait(timeout=10) == 0, "SIGTERM encerra com código 0")
        server = None
    finally:
        if server:
            server.kill()
            server.wait()
        subprocess.run(["dropdb", DB], check=False)
        github.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{'FALHOU' if failures else 'OK'}: {failures} falha(s)")
    return 1 if failures else 0


def _healthy():
    try:
        return call("GET", "/healthz")[0] == 200
    except OSError:
        return False


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build/capi_net_server")))
