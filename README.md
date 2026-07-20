# Raspberry Pi Plate Reader

This repository contains the lightweight Raspberry Pi side of the plate access
control system. The Pi performs only camera capture, YOLO plate detection,
crop/enhancement, and PP-OCRv5 recognition. It sends each result immediately to
the separate PC web server and does not host a website or database.

## Recognition workflow

1. Keep the camera open while YOLO and OCR remain idle.
2. Wait for an administrator to press **Capture plate** on the PC dashboard.
3. Acquire five fresh frames in memory for that queued request.
4. Run YOLO on all five frames and rank the best plate regions.
5. Run PP-OCRv5 on the best three enhanced crops.
6. Choose the final clean alphanumeric plate using OCR consensus.
7. Store only the winning enhanced crop in `Output/Plate-Crops`.
8. Send the plate, detector confidence, and crop to the PC server.

## Raspberry Pi 4 setup

Use 64-bit Raspberry Pi OS and run:

```bash
./build_raspberry_pi.sh
```

The installer adds OpenCV, libcurl, curl, CMake, and the C++ build tools, then builds
the reader with Raspberry Pi 4 CPU tuning.

## Connect it to the PC server

Start the separate `plate-program` project on the PC. That website
uses native MySQL locally; the Pi never connects to MySQL. It sends recognition
results only to the protected Flask API.

Find the PC's local IP address and copy the private value from the web project's
`database/reader_api.key` file. On the Raspberry Pi, run:

```bash
./configure_reader.sh
```

Enter the complete PC website address, such as `http://192.168.0.103:8080`, the
reader API key, and the USB camera index. The configuration is stored in a
private `.env` file that Git ignores. The setup checks the website health route
before accepting the configuration.

Start the reader with:

```bash
./start_reader.sh
```

The launcher checks the PC website before opening the camera, then polls its
capture queue while inference stays idle. The API key stays in the environment
and is not placed in the command line or process listing.

To test only the PC connection without opening the camera:

```bash
./start_reader.sh --check
```

Press **Capture plate** on the administrator dashboard. The Pi terminal prints
the five-frame detection/OCR stages, final plate, web server response, and a
timing summary for frame capture, YOLO, OCR, upload, and total processing time.

For temporary maintenance, the server address and key can still be supplied as
environment variables:

```bash
PLATE_SERVER_URL=http://192.168.0.103:8080 \
PLATE_API_KEY=PASTE_THE_PRIVATE_KEY_HERE \
./build-pi/plate_reader --camera 0 \
  models/license_plate_detector.onnx \
  models/en_PP-OCRv5_rec_mobile.onnx \
  Output --headless
```

The interactive configuration script is preferred because it prevents the key
from being recorded in shell history.

## macOS build

```bash
brew install cmake opencv curl
cmake -S . -B build -DPLATE_ENABLE_CAMERA=ON
cmake --build build -j
```

Run `./configure_reader.sh`, followed by `./start_reader.sh`. The launcher
automatically chooses the macOS build. Camera access must be allowed for Terminal.

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
