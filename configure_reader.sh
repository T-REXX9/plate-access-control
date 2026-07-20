#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config_path="$project_dir/.env"

read -r -p "PC website address (example http://192.168.0.103:8080): " server_url
server_url="${server_url%/}"
if [[ ! "$server_url" =~ ^https?://[^[:space:]]+$ ]]; then
    echo "Enter a complete http:// or https:// website address."
    exit 1
fi

read -r -s -p "Reader API key from the PC website: " api_key
echo
if [[ ${#api_key} -lt 32 ]]; then
    echo "The reader API key is missing or too short."
    exit 1
fi

read -r -p "USB camera index [0]: " camera_index
camera_index="${camera_index:-0}"
if [[ ! "$camera_index" =~ ^[0-9]+$ ]]; then
    echo "Camera index must be zero or a positive number."
    exit 1
fi

umask 077
printf 'PLATE_SERVER_URL=%s\nPLATE_API_KEY=%s\nCAMERA_INDEX=%s\n' \
    "$server_url" "$api_key" "$camera_index" > "$config_path"
chmod 600 "$config_path"

if curl --fail --silent --show-error --connect-timeout 3 --max-time 5 \
    "$server_url/health" >/dev/null; then
    echo "Website connection successful. Reader configuration saved to .env."
else
    echo "Configuration saved, but the website health check failed."
    echo "Confirm the PC address, firewall, and that the website is running."
    exit 1
fi
