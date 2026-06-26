#!/usr/bin/env bash
# Entrypoint for PostgreSQL streaming replica container
# Initializes from primary via pg_basebackup, then starts in standby mode

set -euo pipefail

PGDATA="/var/lib/postgresql/data"
PRIMARY_HOST="${PRIMARY_HOST:-postgres}"
PRIMARY_PORT="${PRIMARY_PORT:-5432}"
REPL_USER="${POSTGRES_USER:-postgres}"
REPL_PASS="${POSTGRES_PASSWORD:-postgres}"

# Wait for primary to be ready
echo "Replica: waiting for primary at ${PRIMARY_HOST}:${PRIMARY_PORT}..."
until PGPASSWORD="$REPL_PASS" pg_isready -h "$PRIMARY_HOST" -p "$PRIMARY_PORT" -U "$REPL_USER" 2>/dev/null; do
    sleep 2
done
echo "Replica: primary is ready."

# If data directory is empty, initialize from primary
if [ ! -f "$PGDATA/PG_VERSION" ]; then
    echo "Replica: no data found, running pg_basebackup..."

    rm -rf "${PGDATA:?}"/*

    PGPASSWORD="$REPL_PASS" pg_basebackup \
        -h "$PRIMARY_HOST" \
        -p "$PRIMARY_PORT" \
        -U "$REPL_USER" \
        -D "$PGDATA" \
        -Fp -Xs -P -R

    # Ensure correct ownership and permissions
    chown -R postgres:postgres "$PGDATA"
    chmod 0700 "$PGDATA"

    echo "Replica: base backup complete."
else
    echo "Replica: existing data found, ensuring standby.signal and primary_conninfo..."
    touch "$PGDATA/standby.signal"
    chown postgres:postgres "$PGDATA/standby.signal"

    # Ensure primary_conninfo is set
    if ! grep -q "primary_conninfo" "$PGDATA/postgresql.auto.conf" 2>/dev/null; then
        echo "primary_conninfo = 'host=${PRIMARY_HOST} port=${PRIMARY_PORT} user=${REPL_USER} password=${REPL_PASS}'" >>"$PGDATA/postgresql.auto.conf"
    fi
fi

echo "Replica: starting PostgreSQL in standby mode..."
exec su-exec postgres postgres
