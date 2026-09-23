-- capi_net — 003: versão do harness por client e histórico de estatísticas.

BEGIN;

-- Versão do fastchess que o client usa (§3.6: o harness precisa ser o mesmo
-- em todos os clients). Reportada no registro.
ALTER TABLE clients ADD COLUMN fastchess_version TEXT;

-- Uma linha por recálculo de test_stats que mudou o número de pares válidos:
-- alimenta o gráfico LLR × pares da web UI. ~15k linhas por teste no máximo.
-- Descarte retroativo (banimento, §7.3) grava uma linha com menos pares.
CREATE TABLE test_stats_history (
    id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    test_id     BIGINT NOT NULL REFERENCES tests(id) ON DELETE CASCADE,
    pairs_valid INTEGER NOT NULL,
    llr         DOUBLE PRECISION,
    elo         DOUBLE PRECISION,
    recorded_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX test_stats_history_test_idx ON test_stats_history (test_id, id);

INSERT INTO schema_migrations (version) VALUES (3);

COMMIT;
