#!/usr/bin/env bash
set -euo pipefail

port="${PLATE_SERVER_PORT:-8080}"
command -v ip >/dev/null 2>&1 || exit 0
command -v nmap >/dev/null 2>&1 || exit 0

networks=()
while read -r address; do
    address="${address%%/*}"
    IFS=. read -r first second third _ <<<"$address"
    if [[ -n "${first:-}" && -n "${second:-}" && -n "${third:-}" ]]; then
        networks+=("$first.$second.$third.0/24")
    fi
done < <(ip -o -4 address show scope global | awk '{print $4}')

found=()
while IFS= read -r network; do
    while read -r host; do
        response="$(curl --silent --connect-timeout 1 --max-time 2 \
            "http://$host:$port/health" 2>/dev/null || true)"
        compact="$(printf '%s' "$response" | tr -d '[:space:]')"
        if [[ "$compact" == *'"service":"plate-program"'* &&
              "$compact" == *'"status":"ok"'* ]]; then
            found+=("http://$host:$port")
        fi
    done < <(
        nmap -n -p "$port" --open --host-timeout 2s -oG - "$network" 2>/dev/null |
            awk -v port="$port" '$0 ~ "Ports: " port "/open/" {print $2}'
    )
done < <(printf '%s\n' "${networks[@]}" | sed '/^$/d' | sort -u)

printf '%s\n' "${found[@]}" | sed '/^$/d' | sort -u
