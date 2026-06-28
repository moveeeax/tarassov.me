#!/usr/bin/env bash
# Runs on the primary during docker-entrypoint-initdb.d to enable replication
# This file is sourced as part of PostgreSQL's init process

set -euo pipefail

# Add a replication entry to pg_hba.conf. Scope it to RFC-1918 ranges that
# cover Docker's default bridge/user-defined networks (172.16.0.0/12 plus
# 192.168.0.0/16) instead of `all`, so the primary won't accept replication
# connections from arbitrary off-network hosts. Tighten further to your
# actual compose subnet (`docker network inspect <net>`) if you pin one.
echo "host replication all 172.16.0.0/12 md5" >>"$PGDATA/pg_hba.conf"
echo "host replication all 192.168.0.0/16 md5" >>"$PGDATA/pg_hba.conf"

echo "Primary: replication access enabled in pg_hba.conf"
