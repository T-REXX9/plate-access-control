# Raspberry Pi Plate Reader

This repository contains the lightweight Raspberry Pi side of the plate access
control system. The Pi performs only camera capture, YOLO plate detection,
crop/enhancement, and PP-OCRv5 recognition. It sends each result immediately to
the separate PC web server and does not host a website or database.

## Recognition workflow

1. Keep the camera open while YOLO and OCR remain idle.
2. On `capture`, acquire five fresh frames in memory.
3. Run YOLO on all five frames and rank the best plate regions.
4. Run PP-OCRv5 on the best three enhanced crops.
5. Choose the final clean alphanumeric plate using OCR consensus.
6. Store only the winning enhanced crop in `Output/Plate-Crops`.
7. Send the plate, detector confidence, and crop to the PC server.

## Raspberry Pi 4 setup

Use 64-bit Raspberry Pi OS and run:

```bash
./build_raspberry_pi.sh
```

The installer adds OpenCV, libcurl, CMake, and the C++ build tools, then builds
the reader with Raspberry Pi 4 CPU tuning.

## Connect it to the PC server

Start the separate `plate-access-control-web` project on the PC. Find the PC's
local IP address and copy the private value from its
`database/reader_api.key` file.

On the Raspberry Pi:

```bash
export PLATE_SERVER_URL=http://192.168.1.100:8080
export PLATE_API_KEY=PASTE_THE_PRIVATE_KEY_HERE

./build-pi/plate_reader --camera 0 \
  models/license_plate_detector.onnx \
  models/en_PP-OCRv5_rec_mobile.onnx \
  Output --headless
```

Replace `192.168.1.100` with the PC's address. Use another camera index if the
USB camera is not index `0`.

At the prompt, enter:

```text
capture
```

The terminal prints the five-frame detection/OCR stages, the final plate, and
the web server response. `status`, `help`, and `quit` remain available.

The server address and key can also be supplied as command-line options:

```bash
./build-pi/plate_reader --camera 0 \
  models/license_plate_detector.onnx \
  models/en_PP-OCRv5_rec_mobile.onnx \
  Output --headless \
  --server-url http://192.168.1.100:8080 \
  --api-key PASTE_THE_PRIVATE_KEY_HERE
```

Environment variables are preferred because they keep the secret out of shell
history and process listings.

## macOS build

```bash
brew install cmake opencv curl
cmake -S . -B build -DPLATE_ENABLE_CAMERA=ON
cmake --build build -j
```

Use the same command as above with `./build/plate_reader`. macOS camera access
must be allowed for Terminal.

## Process an image folder

Folder mode remains available and does not contact the server:

```bash
./build/plate_reader raw-images Output models/license_plate_detector.onnx
```

Annotated images are written to `Output`, and enhanced plate crops are written
to `Output/Plate-Crops`.

## Private data

Do not commit the PC server API key. Models are included so the Pi can run YOLO
and OCR locally without Python, EasyOCR, Tesseract, or an internet connection.
