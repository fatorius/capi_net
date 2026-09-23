-- capi_net — 002: modo degradado para Apple Silicon e adjudicação por teste.

BEGIN;

-- Hosts com núcleos heterogêneos sem afinidade garantida (Apple Silicon):
-- admitidos com poucos slots (P-cores − 2) para não saturar os P-cores; o SO
-- decide o núcleo. Registrado por client para auditoria (plano §3.3, §12).
ALTER TYPE pinning_mode ADD VALUE 'qos_hint';

-- Adjudicação (regras do Fishtest), congelada por teste como os SHAs: todos os
-- clients jogam o teste com as mesmas regras.
--   empate:  a partir do lance 34, |eval| <= 20 cp nas duas engines por
--            8 lances (16 plies) seguidos; zera em avanço de peão ou captura.
--   vitória: |eval| >= 1000 cp nas duas engines por 5 lances (10 plies)
--            seguidos, desde o primeiro lance.
-- NULL em qualquer grupo desliga aquela adjudicação.
ALTER TABLE tests
    ADD COLUMN adj_draw_movenumber   INTEGER DEFAULT 34,
    ADD COLUMN adj_draw_movecount    INTEGER DEFAULT 8,
    ADD COLUMN adj_draw_score_cp     INTEGER DEFAULT 20,
    ADD COLUMN adj_resign_movecount  INTEGER DEFAULT 5,
    ADD COLUMN adj_resign_score_cp   INTEGER DEFAULT 1000,
    ADD COLUMN adj_resign_twosided   BOOLEAN DEFAULT TRUE,
    ADD CHECK ((adj_draw_movenumber IS NULL) = (adj_draw_movecount IS NULL)
               AND (adj_draw_movecount IS NULL) = (adj_draw_score_cp IS NULL)),
    ADD CHECK ((adj_resign_movecount IS NULL) = (adj_resign_score_cp IS NULL)
               AND (adj_resign_score_cp IS NULL) = (adj_resign_twosided IS NULL));

INSERT INTO schema_migrations (version) VALUES (2);

COMMIT;
