#!/usr/bin/env bash
set -euo pipefail

web_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$web_dir/.." && pwd)"
venv_dir="$project_dir/.web-venv"
platform_id="$(uname -s)-$(uname -m)-$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
platform_file="$venv_dir/.platform-id"

if [[ -f "$platform_file" ]] && [[ "$(<"$platform_file")" == "$platform_id" ]]; then
    python3 -m venv "$venv_dir"
else
    python3 -m venv --clear "$venv_dir"
fi
"$venv_dir/bin/python" -m pip install --upgrade pip
"$venv_dir/bin/python" -m pip install -r "$web_dir/requirements.txt"
echo "$platform_id" > "$platform_file"

echo "Website environment ready."
