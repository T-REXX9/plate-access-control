#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
architecture="$(uname -m)"

case "$architecture" in
    aarch64|arm64|armv7l)
        ;;
    *)
        echo "Warning: this installer is intended to run directly on a Raspberry Pi."
        ;;
esac

sudo apt update
sudo apt install -y build-essential cmake pkg-config libopencv-dev libcurl4-openssl-dev curl

cmake \
    -S "$project_dir" \
    -B "$project_dir/build-pi" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPLATE_RASPBERRY_PI=ON

# Two compiler jobs avoid memory pressure on a 4 GB Raspberry Pi while still
# using more than one core.
cmake --build "$project_dir/build-pi" --parallel 2

echo
echo "Build complete. Run:"
echo "cd \"$project_dir\""
echo "./configure_reader.sh"
echo "./start_reader.sh"
