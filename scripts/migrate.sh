#!/usr/bin/env bash
#
# Aplica, em ordem, as migrations de migrations/ ainda não registradas em
# schema_migrations. Idempotente.
#
# Uso: bash scripts/migrate.sh [dbname]     (padrão: capi_net; conexão via PG*)

set -euo pipefail

DB="${1:-capi_net}"
DIR="$(cd "$(dirname "$0")/.." && pwd)/migrations"

psql_q() { psql -X -q -At -v ON_ERROR_STOP=1 -d "$DB" "$@"; }

current=$(psql_q -c "SELECT COALESCE(max(version), 0) FROM schema_migrations" 2>/dev/null || echo 0)

applied=0
for f in "$DIR"/[0-9][0-9][0-9]_*.sql; do
    version=$((10#$(basename "$f" | cut -c1-3)))
    if [ "$version" -gt "$current" ]; then
        echo "aplicando $(basename "$f")"
        psql_q -f "$f"
        applied=$((applied + 1))
    fi
done

echo "schema em $(psql_q -c 'SELECT max(version) FROM schema_migrations') ($applied aplicada(s))"
