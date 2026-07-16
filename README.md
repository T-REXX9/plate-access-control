# C++ License Plate Detection and OCR

This is the C++ conversion of the Python raw-image pipeline. It uses:

- OpenCV DNN with the exported YOLO ONNX model for plate detection
- PP-OCRv5 English ONNX for neural plate-text recognition
- A plate-only crop enlarged before OCR
- Raw uppercase alphanumeric OCR output only; no country-specific formatting,
  spaces, hyphens, punctuation, or filename-specific overrides

## Build on macOS

```bash
brew install cmake opencv
cd converted
cmake -S . -B build -DPLATE_USE_BUNDLED_OPENCV=OFF -DPLATE_ENABLE_CAMERA=ON
cmake --build build -j
```

The full OpenCV package supplies camera capture and the live preview window.
For a batch-only build, the minimal bundled OpenCV runtime remains available
with `-DPLATE_USE_BUNDLED_OPENCV=ON -DPLATE_ENABLE_CAMERA=OFF`.

## Run

Run this command from the `converted` folder:

```bash
./build/plate_reader raw-images Output models/license_plate_detector.onnx
```

The OCR model defaults to `models/en_PP-OCRv5_rec_mobile.onnx`. You can pass a
different compatible OCR model as the optional fifth argument.

Annotated images are saved in `converted/Output`. Zoomed plate-only crops are
saved in `converted/Output/Plate-Crops`.

## Real-time camera

From the `converted` folder, run:

```bash
./build/plate_reader --camera 0
```

The camera remains open, but YOLO and PP-OCRv5 sleep while the marked gate zone
is empty. A lightweight motion gate wakes the models when something enters the
zone and returns to idle afterward. Keep the gate clear during the short
startup calibration. The live scan then uses the same plate crop and skew
correction pipeline as image mode.

Press `Q` or Escape to quit and `S` to save an annotated frame in `Output`. If
macOS asks for camera access, allow it for Terminal (or the app launching the
command).

## Raspberry Pi 4 (4 GB)

Use the current 64-bit Raspberry Pi OS (Trixie or newer), which supplies the
required OpenCV 4.10 package. Copy the complete `converted` folder, including
its `raw-images` folder, to the Pi, then run:

```bash
cd converted
./build_raspberry_pi.sh
./build-pi/plate_reader raw-images Output models/license_plate_detector.onnx
```

For a USB or V4L2 camera on the Pi, start real-time mode with:

```bash
./build-pi/plate_reader --camera 0
```

The Pi build uses the ARM OpenCV package supplied by Raspberry Pi OS rather
than the bundled macOS libraries. It is compiled for the Pi 4 Cortex-A72 CPU
and limits compilation to two parallel jobs so the build remains comfortable
on a 4 GB device. Runtime OCR remains fully local and does not require Python,
Tesseract, EasyOCR, or an internet connection.

## Local database

The portable SQLite database is stored at `database/gate_access.db`. It is
included when the complete `converted` folder is copied to the Raspberry Pi.
Its schema supports registered vehicles, access history, dashboard summaries,
website accounts, audit history, settings, and system health.

To initialize or safely update it:

```bash
./database/init_database.sh
```

The live camera reader checks stable plates against active vehicle records and
writes authorized or denied events, detector confidence, timestamps, and
snapshot paths directly to this database. Duplicate readings of the same plate
are suppressed using the configured time window.

## Local admin website

Install the small website environment once, then start it:

```bash
./web/setup_web.sh
./web/start_web.sh
```

Open `http://localhost:8080` on the same computer. On the first visit, create
the administrator username and password. The site includes the live summary,
registered-vehicle entry and editing, active/inactive authorization controls,
the complete searchable access log, event snapshots, and CSV export. Starting
the website also starts the real-time camera reader in headless mode. Its
annotated feed appears on the dashboard instead of opening a separate window.
The dashboard stream is capped at 5 FPS to reduce Raspberry Pi CPU, disk, and
network use without slowing the internal detection loop.
Recognized registered vehicles are labeled with both the plate and owner, for
example `ZAT255 Melson Bacuen`.

Camera index `0` is used by default. To use another camera, run:

```bash
CAMERA_INDEX=1 ./web/start_web.sh
```

For website-only maintenance without opening a camera, run:

```bash
START_CAMERA=0 ./web/start_web.sh
```

After copying the complete folder to a Raspberry Pi, run
`./build_raspberry_pi.sh`; it installs the Pi dependencies and rebuilds the web
environment for Linux/ARM automatically. Start the site with
`./web/start_web.sh`, then open `http://raspberrypi.local:8080` from another
device on the same local network.
