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

privilege=()
if [[ $EUID -ne 0 ]]; then
    privilege=(sudo)
fi
if [[ "${PLATE_SKIP_APT:-0}" != "1" ]]; then
    "${privilege[@]}" apt update
    "${privilege[@]}" apt install -y build-essential cmake pkg-config libcurl4-openssl-dev libgpiod-dev gpiod curl
fi

opencv_arguments=()
for opencv_config in \
    /opt/opencv-4.10.0/lib/cmake/opencv4/OpenCVConfig.cmake \
    /usr/local/lib/cmake/opencv4/OpenCVConfig.cmake; do
    if [[ -f "$opencv_config" ]]; then
        opencv_arguments+=("-DOpenCV_DIR=$(dirname "$opencv_config")")
        break
    fi
done

cmake \
    -S "$project_dir" \
    -B "$project_dir/build-pi" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPLATE_RASPBERRY_PI=ON \
    -DPLATE_ENABLE_GPIO=ON \
    "${opencv_arguments[@]}"

# Two compiler jobs avoid memory pressure on a 4 GB Raspberry Pi while still
# using more than one core.
cmake --build "$project_dir/build-pi" --parallel 2
ctest --test-dir "$project_dir/build-pi" --output-on-failure

echo
echo "Build complete. Run:"
echo "cd \"$project_dir\""
echo "./configure_reader.sh"
echo "./start_reader.sh"
echo
echo "Gate safety simulator: ./build-pi/gate_simulator"
echo "Wiring guide: docs/GATE_WIRING_DIAGRAM.md"
