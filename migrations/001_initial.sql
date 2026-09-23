-- capi_net — schema inicial (plano técnico v1, §4 e §4.1).
-- Aplicar com: psql -v ON_ERROR_STOP=1 -d <db> -f migrations/001_initial.sql

BEGIN;

CREATE TABLE schema_migrations (
    version     INTEGER PRIMARY KEY,
    applied_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =========================================================
-- TESTES
-- =========================================================
CREATE TYPE test_kind   AS ENUM ('sprt', 'gauntlet');
CREATE TYPE test_status AS ENUM (
    'validating',   -- refs resolvidos, smoke test em andamento
    'invalid',      -- falhou na validação; nunca entra na fila
    'queued', 'running', 'finished', 'stopped'
);
CREATE TYPE sprt_preset AS ENUM ('gainer', 'nonreg', 'custom');
CREATE TYPE test_result AS ENUM ('pending', 'accepted', 'rejected', 'inconclusive');

CREATE TABLE tests (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name                TEXT        NOT NULL,
    kind                test_kind   NOT NULL,
    status              test_status NOT NULL DEFAULT 'validating',
    result              test_result NOT NULL DEFAULT 'pending',

    -- engines (v1: repositório fixo fatorius/capizero, vindo da config do server)
    -- *_ref    = o que o usuário digitou (branch, tag/release ou SHA curto/longo)
    -- *_commit = SHA completo resolvido e CONGELADO na criação do teste
    candidate_ref       TEXT        NOT NULL,
    candidate_commit    CHAR(40)    NOT NULL,
    baseline_ref        TEXT        NOT NULL,
    baseline_commit     CHAR(40)    NOT NULL,
    CHECK (candidate_commit <> baseline_commit),

    -- condições de jogo
    tc_base_ms          INTEGER     NOT NULL,
    tc_increment_ms     INTEGER     NOT NULL,
    hash_mb             INTEGER     NOT NULL DEFAULT 16,
    threads             INTEGER     NOT NULL DEFAULT 1,
    book_name           TEXT        NOT NULL,
    total_pairs         INTEGER     NOT NULL,

    -- parâmetros SPRT (NULL para gauntlet)
    -- Bounds em Elo NORMALIZADO (mesma escala do Fishtest), não Elo logístico.
    sprt_preset         sprt_preset,
    sprt_elo0           DOUBLE PRECISION,
    sprt_elo1           DOUBLE PRECISION,
    sprt_alpha          DOUBLE PRECISION DEFAULT 0.05,
    sprt_beta           DOUBLE PRECISION DEFAULT 0.05,
    CHECK (kind <> 'sprt' OR (sprt_elo0 IS NOT NULL AND sprt_elo1 IS NOT NULL
                              AND sprt_elo0 < sprt_elo1)),

    priority            INTEGER     NOT NULL DEFAULT 0,
    validation_log      TEXT,
    submitted_by        TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    started_at          TIMESTAMPTZ,
    finished_at         TIMESTAMPTZ
);

-- Garante a regra "um teste ativo por vez".
CREATE UNIQUE INDEX one_running_test
    ON tests ((status)) WHERE status = 'running';

-- =========================================================
-- CLIENTS
-- =========================================================
CREATE TYPE client_status AS ENUM ('active', 'idle', 'banned');
CREATE TYPE pinning_mode  AS ENUM ('pinned', 'homogeneous_unpinned');

CREATE TABLE clients (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name                TEXT          NOT NULL UNIQUE,
    hostname            TEXT,
    os                  TEXT,
    cpu_model           TEXT,
    cpu_arch_target     TEXT,
    pinning_mode        pinning_mode  NOT NULL,
    core_topology       TEXT,
    bench_ref_commit    CHAR(40),
    nproc_total         INTEGER,
    slots               INTEGER       NOT NULL,
    cpu_factor          DOUBLE PRECISION,
    status              client_status NOT NULL DEFAULT 'active',
    banned_at           TIMESTAMPTZ,
    ban_reason          TEXT,
    first_seen          TIMESTAMPTZ   NOT NULL DEFAULT now(),
    last_seen           TIMESTAMPTZ   NOT NULL DEFAULT now()
);

-- =========================================================
-- POSIÇÕES DE ABERTURA (geradas por teste, a partir do book)
-- =========================================================
CREATE TABLE opening_positions (
    id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    test_id     BIGINT  NOT NULL REFERENCES tests(id) ON DELETE CASCADE,
    seq         INTEGER NOT NULL,
    fen         TEXT    NOT NULL,
    UNIQUE (test_id, seq)
);

-- =========================================================
-- JOBS  (= PARES de partidas; unidade atômica de trabalho)
-- =========================================================
CREATE TYPE job_status AS ENUM ('pending', 'leased', 'completed', 'discarded');

CREATE TABLE job_pairs (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    test_id             BIGINT     NOT NULL REFERENCES tests(id) ON DELETE CASCADE,
    position_id         BIGINT     NOT NULL REFERENCES opening_positions(id),
    status              job_status NOT NULL DEFAULT 'pending',
    dispatch_order      DOUBLE PRECISION NOT NULL,

    leased_to           BIGINT     REFERENCES clients(id),
    leased_at           TIMESTAMPTZ,
    lease_expires_at    TIMESTAMPTZ,
    attempts            INTEGER    NOT NULL DEFAULT 0,

    completed_at        TIMESTAMPTZ,
    discarded_reason    TEXT,
    requeued_from       BIGINT REFERENCES job_pairs(id)
);

-- Sem UNIQUE (test_id, position_id): o re-enfileiramento após banimento cria um
-- NOVO par para a mesma posição (§7.3). Invariante: no máximo um par VIVO.
CREATE UNIQUE INDEX one_live_pair_per_position
    ON job_pairs (test_id, position_id)
    WHERE status <> 'discarded';

CREATE INDEX job_pairs_claim_idx
    ON job_pairs (test_id, status, dispatch_order)
    WHERE status = 'pending';

CREATE INDEX job_pairs_lease_idx
    ON job_pairs (lease_expires_at) WHERE status = 'leased';

CREATE INDEX job_pairs_client_idx ON job_pairs (leased_to);

-- =========================================================
-- PARTIDAS (tabela quente — lida inteira a cada recálculo)
-- =========================================================
CREATE TYPE game_outcome     AS ENUM ('candidate_win', 'draw', 'candidate_loss');
CREATE TYPE game_termination AS ENUM (
    'normal', 'timeout', 'illegal_move', 'crash', 'adjudication', 'other'
);

CREATE TABLE games (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    pair_id             BIGINT  NOT NULL REFERENCES job_pairs(id) ON DELETE CASCADE,
    test_id             BIGINT  NOT NULL REFERENCES tests(id) ON DELETE CASCADE,
    client_id           BIGINT  NOT NULL REFERENCES clients(id),

    slot_index          INTEGER NOT NULL,
    core_id             INTEGER,
    game_in_pair        SMALLINT NOT NULL CHECK (game_in_pair IN (0, 1)),
    candidate_is_white  BOOLEAN NOT NULL,

    outcome             game_outcome     NOT NULL,
    termination         game_termination NOT NULL,
    ply_count           INTEGER,
    duration_ms         INTEGER,

    cpu_factor_at_play  DOUBLE PRECISION NOT NULL,
    tc_base_effective_ms      INTEGER NOT NULL,
    tc_increment_effective_ms INTEGER NOT NULL,

    valid               BOOLEAN NOT NULL DEFAULT TRUE,
    invalid_reason      TEXT,

    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (pair_id, game_in_pair)
);

CREATE INDEX games_test_valid_idx ON games (test_id) WHERE valid;
CREATE INDEX games_client_idx     ON games (client_id);

-- =========================================================
-- PGN (tabela fria, separada, comprimida)
-- =========================================================
CREATE TABLE game_pgns (
    game_id     BIGINT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
    pgn_gz      BYTEA NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =========================================================
-- SNAPSHOTS DE ESTATÍSTICA
-- =========================================================
CREATE TABLE test_stats (
    test_id         BIGINT PRIMARY KEY REFERENCES tests(id) ON DELETE CASCADE,
    pairs_valid     INTEGER NOT NULL,
    penta_ll        INTEGER NOT NULL,
    penta_ld        INTEGER NOT NULL,
    penta_dd_wl     INTEGER NOT NULL,
    penta_wd        INTEGER NOT NULL,
    penta_ww        INTEGER NOT NULL,
    llr             DOUBLE PRECISION,
    llr_lower       DOUBLE PRECISION,
    llr_upper       DOUBLE PRECISION,
    elo             DOUBLE PRECISION,   -- Elo logístico (common/elo.hpp)
    elo_ci_low      DOUBLE PRECISION,
    elo_ci_high     DOUBLE PRECISION,
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =========================================================
-- RESÍDUO POR CLIENT (interno, base para banimento automático)
-- =========================================================
CREATE TABLE client_residuals (
    test_id         BIGINT NOT NULL REFERENCES tests(id) ON DELETE CASCADE,
    client_id       BIGINT NOT NULL REFERENCES clients(id),
    pairs_played    INTEGER NOT NULL,
    chi2            DOUBLE PRECISION,
    residual        DOUBLE PRECISION,
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (test_id, client_id)
);

-- =========================================================
-- VIEW DE AGREGAÇÃO PENTANOMIAL (§4.1)
-- pair_score: 0.0 → LL, 0.5 → LD, 1.0 → DD/WL, 1.5 → WD, 2.0 → WW
-- =========================================================
CREATE VIEW pair_outcomes AS
SELECT
    p.id            AS pair_id,
    p.test_id,
    g.client_id,
    SUM(CASE g.outcome
            WHEN 'candidate_win'  THEN 1.0
            WHEN 'draw'           THEN 0.5
            ELSE 0.0
        END) AS pair_score
FROM job_pairs p
JOIN games g ON g.pair_id = p.id
WHERE p.status = 'completed' AND g.valid
GROUP BY p.id, p.test_id, g.client_id
HAVING COUNT(*) = 2;

INSERT INTO schema_migrations (version) VALUES (1);

COMMIT;
