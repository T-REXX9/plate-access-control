#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config_path="${PLATE_READER_CONFIG:-$project_dir/.env}"

if [[ ! -f "$config_path" ]]; then
    echo "Reader configuration is missing. Run ./configure_reader.sh first."
    exit 1
fi

set -a
# shellcheck disable=SC1090
source "$config_path"
set +a

: "${PLATE_SERVER_URL:?PLATE_SERVER_URL is missing from .env}"
: "${CAMERA_INDEX:=0}"
: "${GATE_MODE:=0}"
: "${PLATE_OUTPUT_DIR:=$project_dir/Output}"

if [[ "$GATE_MODE" != "0" && "$GATE_MODE" != "1" ]]; then
    echo "GATE_MODE must be 0 or 1."
    exit 1
fi

PLATE_SERVER_URL="${PLATE_SERVER_URL%/}"
export PLATE_SERVER_URL CAMERA_INDEX
mkdir -p "$PLATE_OUTPUT_DIR"

if ! curl --fail --silent --show-error --connect-timeout 3 --max-time 5 \
    "$PLATE_SERVER_URL/health" >/dev/null; then
    echo "The PC website is unavailable at $PLATE_SERVER_URL."
    echo "The reader was not started."
    exit 1
fi

if [[ "${1:-}" == "--check" ]]; then
    echo "PC website connection successful: $PLATE_SERVER_URL"
    exit 0
fi

reader="$project_dir/build-pi/plate_reader"
if [[ "$(uname -s)" == "Darwin" ]]; then
    reader="$project_dir/build/plate_reader"
fi
if [[ ! -x "$reader" ]]; then
    echo "The plate reader is not built. Run ./build_raspberry_pi.sh first."
    exit 1
fi

cd "$project_dir"
echo "PC website connected: $PLATE_SERVER_URL"
mode_argument="--remote-commands"
if [[ "$GATE_MODE" == "1" ]]; then
    mode_argument="--gate"
    echo "Starting automatic gate mode on camera $CAMERA_INDEX."
else
    echo "Starting camera $CAMERA_INDEX in idle mode; Capture requests come from the website."
fi
exec "$reader" --camera "$CAMERA_INDEX" \
  models/license_plate_detector.onnx \
  models/en_PP-OCRv5_rec_mobile.onnx \
  "$PLATE_OUTPUT_DIR" --headless "$mode_argument"
