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

camera_pid=""
web_pid=""
camera_pid_file="$project_dir/Output/camera.pid"

cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$web_pid" ]] && kill -0 "$web_pid" 2>/dev/null; then
        kill "$web_pid" 2>/dev/null || true
        wait "$web_pid" 2>/dev/null || true
    fi
    if [[ -n "$camera_pid" ]] && kill -0 "$camera_pid" 2>/dev/null; then
        kill "$camera_pid" 2>/dev/null || true
        wait "$camera_pid" 2>/dev/null || true
    fi
    rm -f "$camera_pid_file"
}
trap cleanup EXIT INT TERM

if [[ "${START_CAMERA:-1}" != "0" ]]; then
    reader="$project_dir/build/plate_reader"
    if [[ "$(uname -s)" == "Linux" ]] && [[ -x "$project_dir/build-pi/plate_reader" ]]; then
        reader="$project_dir/build-pi/plate_reader"
    fi
    if [[ -x "$reader" ]]; then
        mkdir -p "$project_dir/Output"
        cd "$project_dir"
        "$reader" --camera "${CAMERA_INDEX:-0}" \
            models/license_plate_detector.onnx \
            models/en_PP-OCRv5_rec_mobile.onnx \
            Output database/gate_access.db --headless \
            >> Output/camera.log 2>&1 &
        camera_pid=$!
        echo "$camera_pid" > "$camera_pid_file"
        echo "Camera reader started (PID $camera_pid)."
    else
        echo "Warning: plate_reader is not built; website will start without the camera."
    fi
fi

cd "$web_dir"
echo "Admin website: http://0.0.0.0:8080"
"$venv_dir/bin/waitress-serve" --listen=0.0.0.0:8080 app:app &
web_pid=$!
wait "$web_pid"
