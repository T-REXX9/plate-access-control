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
: "${PLATE_API_KEY:?PLATE_API_KEY is missing from .env}"
: "${CAMERA_INDEX:=0}"

PLATE_SERVER_URL="${PLATE_SERVER_URL%/}"
export PLATE_SERVER_URL PLATE_API_KEY CAMERA_INDEX

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
echo "Starting camera $CAMERA_INDEX in idle on-demand mode."
exec "$reader" --camera "$CAMERA_INDEX" \
    models/license_plate_detector.onnx \
    models/en_PP-OCRv5_rec_mobile.onnx \
    Output --headless
