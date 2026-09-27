-- capi_net — 005: placar (V/E/D) nas estatísticas e resultado 'completed'.

-- Gauntlet não tem veredito de SPRT: ao consumir todos os pares o resultado
-- passa a ser 'completed' (antes ficava 'pending'). ADD VALUE fica numa
-- transação própria porque o valor novo não pode ser usado na mesma transação.
BEGIN;
ALTER TYPE test_result ADD VALUE IF NOT EXISTS 'completed';
COMMIT;

BEGIN;

UPDATE tests SET result = 'completed'
WHERE kind = 'gauntlet' AND status = 'finished' AND result = 'pending';

-- Placar do candidate nas partidas que entram na estatística (pares com as
-- duas partidas válidas, os mesmos da distribuição pentanomial).
ALTER TABLE test_stats
    ADD COLUMN games_wins   INTEGER NOT NULL DEFAULT 0,
    ADD COLUMN games_draws  INTEGER NOT NULL DEFAULT 0,
    ADD COLUMN games_losses INTEGER NOT NULL DEFAULT 0;

UPDATE test_stats s SET
    games_wins   = sc.wins,
    games_draws  = sc.draws,
    games_losses = sc.losses
FROM (
    SELECT g.test_id,
           count(*) FILTER (WHERE g.outcome = 'candidate_win')  AS wins,
           count(*) FILTER (WHERE g.outcome = 'draw')           AS draws,
           count(*) FILTER (WHERE g.outcome = 'candidate_loss') AS losses
    FROM games g
    JOIN pair_outcomes po ON po.pair_id = g.pair_id
    WHERE g.valid
    GROUP BY g.test_id
) sc
WHERE sc.test_id = s.test_id;

INSERT INTO schema_migrations (version) VALUES (5);

COMMIT;
