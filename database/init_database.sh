#!/usr/bin/env bash
set -euo pipefail

database_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
database_path="$database_dir/gate_access.db"
schema_path="$database_dir/schema.sql"

if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "sqlite3 is required. On Raspberry Pi OS run: sudo apt install sqlite3"
    exit 1
fi

sqlite3 "$database_path" < "$schema_path"
echo "Database ready: $database_path"
