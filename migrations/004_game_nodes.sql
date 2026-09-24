-- capi_net — 004: nós e tempo de busca por engine em cada partida.
-- Somados pelo client a partir dos comentários do fastchess (-pgnout
-- nodes=true). NPS = nodes / time. NULL em partidas sem essa informação.

BEGIN;

ALTER TABLE games
    ADD COLUMN candidate_nodes   BIGINT,
    ADD COLUMN candidate_time_ms BIGINT,
    ADD COLUMN baseline_nodes    BIGINT,
    ADD COLUMN baseline_time_ms  BIGINT;

INSERT INTO schema_migrations (version) VALUES (4);

COMMIT;
