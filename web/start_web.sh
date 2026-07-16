#!/usr/bin/env bash
set -euo pipefail

web_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$web_dir/.." && pwd)"
venv_dir="$project_dir/.web-venv"
platform_id="$(uname -s)-$(uname -m)-$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
platform_file="$venv_dir/.platform-id"

if [[ ! -x "$venv_dir/bin/waitress-serve" ]] ||
   [[ ! -f "$platform_file" ]] ||
   [[ "$(<"$platform_file")" != "$platform_id" ]]; then
    "$web_dir/setup_web.sh"
fi

cd "$web_dir"
echo "Admin website: http://0.0.0.0:8080"
exec "$venv_dir/bin/waitress-serve" --listen=0.0.0.0:8080 app:app
