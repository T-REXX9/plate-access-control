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
cmake -S . -B build -DPLATE_ENABLE_CAMERA=ON
cmake --build build -j
```

The Homebrew OpenCV package supplies detection, OCR preprocessing, camera
capture, and the optional local preview window. Platform-specific OpenCV
binaries are intentionally not stored in this repository.

## Run

Run this command from the `converted` folder:

```bash
./build/plate_reader raw-images Output models/license_plate_detector.onnx
```

The OCR model defaults to `models/en_PP-OCRv5_rec_mobile.onnx`. You can pass a
different compatible OCR model as the optional fifth argument.

Annotated images are saved in `converted/Output`. Zoomed plate-only crops are
saved in `converted/Output/Plate-Crops`.

## On-demand camera recognition

From the `converted` folder, run:

```bash
./build/plate_reader --camera 0
```

The camera remains open while YOLO and PP-OCRv5 stay completely idle. At the
`plate-reader>` prompt, enter `capture` to acquire one fresh photo and run the
complete YOLO detection, plate crop, enhancement, skew correction, OCR, and
database authorization pipeline. The reader prints every processing stage and
returns to idle when the capture finishes. Enter `status`, `help`, or `quit`
for the other available commands.

Requested full frames remain in memory only and are discarded as soon as each
capture finishes. Enhanced plate crops are stored in `Output/Plate-Crops` and
database events reference those crops. The dashboard keeps one overwritten
`Output/latest-capture.jpg` annotated preview; it is never archived per event.
If macOS asks for camera access, allow it for Terminal (or the app launching
the command).

## Raspberry Pi 4 (4 GB)

Use the current 64-bit Raspberry Pi OS (Trixie or newer), which supplies the
required OpenCV 4.10 package. Copy the complete `converted` folder, including
its `raw-images` folder, to the Pi, then run:

```bash
cd converted
./build_raspberry_pi.sh
./build-pi/plate_reader raw-images Output models/license_plate_detector.onnx
```

For a USB or V4L2 camera on the Pi, start on-demand headless mode with:

```bash
./build-pi/plate_reader --camera 0 \
  models/license_plate_detector.onnx \
  models/en_PP-OCRv5_rec_mobile.onnx \
  Output database/gate_access.db --headless
```

Enter `capture` whenever the gate controller requests a recognition attempt.
No YOLO or OCR inference occurs between capture commands, keeping idle CPU and
memory use low on the Raspberry Pi 4.

The Pi build uses the ARM OpenCV package supplied by Raspberry Pi OS. It is
compiled for the Pi 4 Cortex-A72 CPU and limits compilation to two parallel
jobs so the build remains comfortable on a 4 GB device. Runtime OCR remains
fully local and does not require Python, Tesseract, EasyOCR, or an internet
connection.

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
the website also starts the on-demand camera reader in headless idle mode. The
administrator uses the `Capture plate` button to request one fresh frame and
one YOLO → crop/enhancement → OCR pass. The latest annotated preview then
appears automatically without continuously streaming video or running idle
inference.
Recognized registered vehicles are labeled with both the plate and owner, for
example `ZAT255 Melson Bacuen`; denied unregistered vehicles retain the plate
annotation without an owner name.

The dashboard provides Capture plate, Start recognition, and Stop recognition
controls for the actual camera process. System status and seven-day activity stay at the top,
and the desktop overview is arranged to fit common laptop screens without page
scrolling. The full access history remains available through `View all`.
Dashboard counters, status, the latest capture, recent access, and seven-day
activity synchronize in the background every two seconds without reloading the
page or resetting its current position. Synchronization pauses in hidden tabs.

Administrators can create read-only security-guard accounts from the `Guards`
page. A guard can view only the overview, latest annotated event photo, and
access logs. Vehicle administration, camera Start/Stop controls, CSV export,
and guard-account management are hidden and rejected server-side. Deactivating
a guard account also invalidates its existing login session.

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
