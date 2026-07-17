#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#ifdef PLATE_ENABLE_CAMERA
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>
#include <sqlite3.h>
#endif

namespace fs = std::filesystem;

constexpr int kInputSize = 640;
constexpr float kConfidence = 0.55F;
constexpr float kNmsThreshold = 0.50F;

struct LetterboxResult {
    cv::Mat image;
    float scale;
    int padX;
    int padY;
};

struct Detection {
    cv::Rect box;
    float confidence;
};

LetterboxResult letterbox(const cv::Mat& source) {
    const float scale = std::min(
        static_cast<float>(kInputSize) / source.cols,
        static_cast<float>(kInputSize) / source.rows
    );
    const int resizedWidth = static_cast<int>(std::round(source.cols * scale));
    const int resizedHeight = static_cast<int>(std::round(source.rows * scale));
    const int padX = (kInputSize - resizedWidth) / 2;
    const int padY = (kInputSize - resizedHeight) / 2;

    cv::Mat resized;
    cv::resize(source, resized, cv::Size(resizedWidth, resizedHeight));
    cv::Mat padded(kInputSize, kInputSize, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(padX, padY, resizedWidth, resizedHeight)));
    return {padded, scale, padX, padY};
}

std::vector<Detection> detectPlates(cv::dnn::Net& network, const cv::Mat& source) {
    const LetterboxResult input = letterbox(source);
    cv::Mat blob = cv::dnn::blobFromImage(
        input.image,
        1.0 / 255.0,
        cv::Size(kInputSize, kInputSize),
        cv::Scalar(),
        true,
        false
    );
    network.setInput(blob);
    cv::Mat output = network.forward();

    cv::Mat rows;
    if (output.dims == 3 && output.size[1] < output.size[2]) {
        cv::Mat channels(output.size[1], output.size[2], CV_32F, output.ptr<float>());
        cv::transpose(channels, rows);
    } else if (output.dims == 3) {
        rows = cv::Mat(output.size[1], output.size[2], CV_32F, output.ptr<float>());
    } else {
        rows = output;
    }

    std::vector<cv::Rect> boxes;
    std::vector<float> scores;
    for (int index = 0; index < rows.rows; ++index) {
        const float* values = rows.ptr<float>(index);
        const float score = values[4];
        if (score < kConfidence) {
            continue;
        }

        const float centerX = (values[0] - input.padX) / input.scale;
        const float centerY = (values[1] - input.padY) / input.scale;
        const float width = values[2] / input.scale;
        const float height = values[3] / input.scale;
        int left = static_cast<int>(std::round(centerX - width / 2));
        int top = static_cast<int>(std::round(centerY - height / 2));
        int right = static_cast<int>(std::round(centerX + width / 2));
        int bottom = static_cast<int>(std::round(centerY + height / 2));

        left = std::clamp(left, 0, source.cols - 1);
        top = std::clamp(top, 0, source.rows - 1);
        right = std::clamp(right, left + 1, source.cols);
        bottom = std::clamp(bottom, top + 1, source.rows);
        boxes.emplace_back(left, top, right - left, bottom - top);
        scores.push_back(score);
    }

    std::vector<int> kept;
    cv::dnn::NMSBoxes(boxes, scores, kConfidence, kNmsThreshold, kept);
    std::vector<Detection> detections;
    for (const int index : kept) {
        detections.push_back({boxes[index], scores[index]});
    }
    std::sort(detections.begin(), detections.end(), [](const Detection& a, const Detection& b) {
        if (a.box.x == b.box.x) {
            return a.box.y < b.box.y;
        }
        return a.box.x < b.box.x;
    });
    return detections;
}

cv::Mat zoomPlate(const cv::Mat& crop, int targetWidth = 800) {
    const double scale = std::clamp(
        static_cast<double>(targetWidth) / std::max(1, crop.cols),
        1.0,
        6.0
    );
    cv::Mat zoomed;
    cv::resize(crop, zoomed, cv::Size(), scale, scale, cv::INTER_CUBIC);
    return zoomed;
}

std::string cleanPlateText(const std::string& raw) {
    std::string cleaned;
    for (const unsigned char character : raw) {
        if (std::isalnum(character)) {
            cleaned.push_back(static_cast<char>(std::toupper(character)));
        }
    }
    return cleaned;
}

char paddleCharacter(int index) {
    // PP-OCRv5 English classes: blank, 0-9, A-Z, a-z, then punctuation.
    if (index >= 1 && index <= 10) {
        return static_cast<char>('0' + index - 1);
    }
    if (index >= 11 && index <= 36) {
        return static_cast<char>('A' + index - 11);
    }
    if (index >= 37 && index <= 62) {
        return static_cast<char>('a' + index - 37);
    }
    return '\0';
}

double estimatePlateSkew(const cv::Mat& crop) {
    cv::Mat gray;
    cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
    cv::Mat edges;
    cv::Canny(gray, edges, 50, 150);

    std::vector<cv::Vec4i> lines;
    cv::HoughLinesP(
        edges,
        lines,
        1.0,
        CV_PI / 180.0,
        std::max(25, crop.cols / 8),
        crop.cols * 0.30,
        crop.cols * 0.08
    );

    std::vector<std::pair<double, double>> angles;
    double totalWeight = 0.0;
    for (const cv::Vec4i& line : lines) {
        const double dx = line[2] - line[0];
        const double dy = line[3] - line[1];
        const double angle = std::atan2(dy, dx) * 180.0 / CV_PI;
        if (std::abs(angle) <= 30.0) {
            const double weight = std::hypot(dx, dy);
            angles.emplace_back(angle, weight);
            totalWeight += weight;
        }
    }
    if (angles.empty()) {
        return 0.0;
    }

    std::sort(angles.begin(), angles.end());
    double accumulated = 0.0;
    for (const auto& [angle, weight] : angles) {
        accumulated += weight;
        if (accumulated >= totalWeight / 2.0) {
            return angle;
        }
    }
    return angles.back().first;
}

std::string readPlate(cv::dnn::Net& recognizer, const cv::Mat& zoomedCrop) {
    constexpr int inputHeight = 48;
    constexpr int inputWidth = 320;
    const double skew = estimatePlateSkew(zoomedCrop);
    cv::Mat aligned = zoomedCrop;
    bool correctedSkew = false;
    if (std::abs(skew) > 8.0) {
        const cv::Point2f center(zoomedCrop.cols / 2.0F, zoomedCrop.rows / 2.0F);
        const double radians = skew * 0.5 * CV_PI / 180.0;
        const double alpha = std::cos(radians);
        const double beta = std::sin(radians);
        cv::Mat transform(2, 3, CV_64F);
        transform.at<double>(0, 0) = alpha;
        transform.at<double>(0, 1) = beta;
        transform.at<double>(0, 2) = (1.0 - alpha) * center.x - beta * center.y;
        transform.at<double>(1, 0) = -beta;
        transform.at<double>(1, 1) = alpha;
        transform.at<double>(1, 2) = beta * center.x + (1.0 - alpha) * center.y;
        cv::warpAffine(
            zoomedCrop,
            aligned,
            transform,
            zoomedCrop.size(),
            cv::INTER_CUBIC,
            cv::BORDER_CONSTANT,
            cv::Scalar(0, 0, 0)
        );
        correctedSkew = true;
    }

    // Remove borders, state names, slogans, and stickers while retaining the
    // large registration row. This is geometry-only and does not assume a
    // country-specific plate-number format.
    const double topRatio = correctedSkew ? 0.10 : 0.05;
    const double bottomRatio = correctedSkew ? 0.80 : 0.85;
    const int top = static_cast<int>(aligned.rows * topRatio);
    const int bottom = std::max(top + 1, static_cast<int>(aligned.rows * bottomRatio));
    const cv::Mat registrationBand = aligned(
        cv::Rect(0, top, aligned.cols, std::min(bottom, aligned.rows) - top)
    );

    const double ratio = static_cast<double>(registrationBand.cols) / registrationBand.rows;
    const int resizedWidth = std::min(
        inputWidth,
        static_cast<int>(std::ceil(inputHeight * ratio))
    );

    cv::Mat resized;
    cv::resize(registrationBand, resized, cv::Size(resizedWidth, inputHeight));
    resized.convertTo(resized, CV_32FC3, 1.0 / 127.5, -1.0);
    cv::Mat padded(inputHeight, inputWidth, CV_32FC3, cv::Scalar(0, 0, 0));
    resized.copyTo(padded(cv::Rect(0, 0, resizedWidth, inputHeight)));

    const cv::Mat blob = cv::dnn::blobFromImage(padded, 1.0, cv::Size(), cv::Scalar(), false, false);
    recognizer.setInput(blob);
    const cv::Mat output = recognizer.forward();
    if (output.dims != 3 || output.size[0] != 1) {
        return "UNREADABLE";
    }

    const int timeSteps = output.size[1];
    const int classCount = output.size[2];
    const float* probabilities = output.ptr<float>();
    std::string decoded;
    int previous = -1;
    for (int step = 0; step < timeSteps; ++step) {
        const float* row = probabilities + step * classCount;
        const int index = static_cast<int>(std::max_element(row, row + classCount) - row);
        if (index != previous && index != 0) {
            const char character = paddleCharacter(index);
            if (character != '\0') {
                decoded.push_back(character);
            }
        }
        previous = index;
    }

    const std::string cleaned = cleanPlateText(decoded);
    return cleaned.empty() ? "UNREADABLE" : cleaned;
}

void drawLabel(
    cv::Mat& image,
    const cv::Rect& box,
    const std::string& text,
    const cv::Scalar& color = cv::Scalar(0, 255, 0)
) {
    cv::rectangle(image, box, color, 3);
    const double scale = std::max(0.6, std::min(image.cols, image.rows) / 900.0);
    const int thickness = std::max(1, static_cast<int>(std::round(scale * 2)));
    int baseline = 0;
    const cv::Size textSize = cv::getTextSize(
        text,
        cv::FONT_HERSHEY_SIMPLEX,
        scale,
        thickness,
        &baseline
    );
    const int labelY = std::max(textSize.height + baseline + 8, box.y);
    cv::rectangle(
        image,
        cv::Rect(box.x, labelY - textSize.height - baseline - 8, textSize.width + 12, textSize.height + baseline + 8),
        cv::Scalar(color[0] * 0.45, color[1] * 0.45, color[2] * 0.45),
        cv::FILLED
    );
    cv::putText(
        image,
        text,
        cv::Point(box.x + 6, labelY - baseline - 4),
        cv::FONT_HERSHEY_SIMPLEX,
        scale,
        cv::Scalar(255, 255, 255),
        thickness,
        cv::LINE_AA
    );
}

bool supportedImage(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png" ||
           extension == ".webp" || extension == ".avif";
}

#ifdef PLATE_ENABLE_CAMERA
struct AuthorizationResult {
    bool authorized = false;
    int vehicleId = -1;
    std::string ownerName;
};

class GateDatabase {
public:
    explicit GateDatabase(const fs::path& path) {
        if (sqlite3_open_v2(
                path.string().c_str(),
                &database_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                nullptr
            ) != SQLITE_OK) {
            std::cerr << "Database unavailable: "
                      << (database_ ? sqlite3_errmsg(database_) : "unable to open file")
                      << '\n';
            if (database_) {
                sqlite3_close(database_);
                database_ = nullptr;
            }
            return;
        }
        sqlite3_busy_timeout(database_, 10000);
        sqlite3_exec(database_, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);
        sqlite3_exec(database_, "PRAGMA journal_mode = WAL", nullptr, nullptr, nullptr);
    }

    ~GateDatabase() {
        if (database_) {
            sqlite3_close(database_);
        }
    }

    bool available() const {
        return database_ != nullptr;
    }

    AuthorizationResult authorize(const std::string& plate) {
        AuthorizationResult result;
        if (!database_) {
            return result;
        }
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "SELECT id, owner_name FROM vehicles "
            "WHERE plate_number = ? AND is_active = 1 "
            "AND (registration_expires_on IS NULL OR registration_expires_on = '' "
            "OR date(registration_expires_on) >= date('now', 'localtime')) "
            "LIMIT 1";
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, plate.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement) == SQLITE_ROW) {
                result.authorized = true;
                result.vehicleId = sqlite3_column_int(statement, 0);
                const unsigned char* owner = sqlite3_column_text(statement, 1);
                if (owner) {
                    result.ownerName = reinterpret_cast<const char*>(owner);
                }
            }
        }
        sqlite3_finalize(statement);
        return result;
    }

    bool isRecentDuplicate(const std::string& plate) {
        if (!database_) {
            return false;
        }
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "SELECT 1 FROM access_events "
            "WHERE plate_number = ? AND detected_at >= datetime(" 
            "'now', '-' || coalesce((SELECT value FROM settings "
            "WHERE key = 'duplicate_event_seconds'), '30') || ' seconds') LIMIT 1";
        bool duplicate = false;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, plate.c_str(), -1, SQLITE_TRANSIENT);
            duplicate = sqlite3_step(statement) == SQLITE_ROW;
        }
        sqlite3_finalize(statement);
        return duplicate;
    }

    bool recordEvent(
        const std::string& plate,
        const AuthorizationResult& authorization,
        float detectorConfidence,
        const std::string& imagePath
    ) {
        if (!database_ || isRecentDuplicate(plate)) {
            return false;
        }
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "INSERT INTO access_events "
            "(vehicle_id, plate_number, decision, gate_action, detector_confidence, image_path) "
            "VALUES (?, ?, ?, 'not_requested', ?, ?)";
        bool inserted = false;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK) {
            if (authorization.vehicleId >= 0) {
                sqlite3_bind_int(statement, 1, authorization.vehicleId);
            } else {
                sqlite3_bind_null(statement, 1);
            }
            sqlite3_bind_text(statement, 2, plate.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(
                statement,
                3,
                authorization.authorized ? "authorized" : "denied",
                -1,
                SQLITE_STATIC
            );
            sqlite3_bind_double(statement, 4, detectorConfidence);
            sqlite3_bind_text(statement, 5, imagePath.c_str(), -1, SQLITE_TRANSIENT);
            inserted = sqlite3_step(statement) == SQLITE_DONE;
        }
        sqlite3_finalize(statement);
        return inserted;
    }

    void updateStatus(
        const std::string& cameraState,
        const std::string& detectorState,
        const std::string& lastPlate = ""
    ) {
        if (!database_) {
            return;
        }
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "UPDATE system_status SET camera_state = ?, detector_state = ?, "
            "last_plate = CASE WHEN ? = '' THEN last_plate ELSE ? END, "
            "last_heartbeat = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE id = 1";
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, cameraState.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, detectorState.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, lastPlate.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 4, lastPlate.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(statement);
        }
        sqlite3_finalize(statement);
    }

private:
    sqlite3* database_ = nullptr;
};

std::string eventSnapshotName(const std::string& plate, int frameNumber) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &timestamp);
#else
    localtime_r(&timestamp, &localTime);
#endif
    std::ostringstream name;
    name << std::put_time(&localTime, "%Y%m%d-%H%M%S")
         << '-' << plate << '-' << frameNumber << ".jpg";
    return name.str();
}

int runCamera(
    cv::dnn::Net& detector,
    cv::dnn::Net& recognizer,
    int cameraIndex,
    const fs::path& outputDirectory,
    const fs::path& databasePath,
    bool headless,
    const fs::path& commandFile
) {
    std::cout << std::unitbuf;
    cv::VideoCapture camera;
#ifdef __APPLE__
    camera.open(cameraIndex, cv::CAP_AVFOUNDATION);
#else
    camera.open(cameraIndex, cv::CAP_V4L2);
#endif
    if (!camera.isOpened()) {
        camera.open(cameraIndex);
    }
    if (!camera.isOpened()) {
        std::cerr << "Unable to open camera " << cameraIndex << ".\n";
        return 1;
    }

    camera.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    camera.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
    camera.set(cv::CAP_PROP_BUFFERSIZE, 1);
    fs::create_directories(outputDirectory);
    const fs::path cropDirectory = outputDirectory / "Plate-Crops";
    const fs::path latestCapturePath = outputDirectory / "latest-capture.jpg";
    fs::create_directories(cropDirectory);
    if (!commandFile.empty()) {
        if (!commandFile.parent_path().empty()) {
            fs::create_directories(commandFile.parent_path());
        }
        std::error_code error;
        fs::remove(commandFile, error);
    }
    GateDatabase gateDatabase(databasePath);
    if (!gateDatabase.available()) {
        std::cerr << "Recognition will continue, but access events cannot be recorded.\n";
    }

    if (!headless) {
        cv::namedWindow("On-demand License Plate Recognition", cv::WINDOW_NORMAL);
    }
    gateDatabase.updateStatus("online", "idle");
    std::cout
        << "Camera ready in on-demand mode. YOLO and OCR are idle.\n"
        << "Commands: capture | status | help | quit\n";
    if (!commandFile.empty()) {
        std::cout << "Waiting for website commands in " << commandFile.string() << ".\n";
    }

    int captureNumber = 0;
    std::string command;
    auto lastHeartbeat = std::chrono::steady_clock::now();
    while (true) {
        command.clear();
        if (commandFile.empty()) {
            std::cout << "plate-reader> " << std::flush;
            if (!std::getline(std::cin, command)) {
                break;
            }
        } else {
            while (command.empty()) {
                std::ifstream input(commandFile);
                if (input) {
                    std::getline(input, command);
                    input.close();
                    std::error_code error;
                    fs::remove(commandFile, error);
                } else {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastHeartbeat >= std::chrono::seconds(1)) {
                        gateDatabase.updateStatus("online", "idle");
                        lastHeartbeat = now;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        command.erase(command.begin(), std::find_if(command.begin(), command.end(), [](unsigned char c) {
            return !std::isspace(c);
        }));
        command.erase(std::find_if(command.rbegin(), command.rend(), [](unsigned char c) {
            return !std::isspace(c);
        }).base(), command.end());
        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (command == "quit" || command == "exit" || command == "q") {
            break;
        }
        if (command == "help") {
            std::cout
                << "capture  Grab one fresh frame and run YOLO, crop enhancement, and OCR.\n"
                << "status   Show whether the camera and models are idle.\n"
                << "quit     Release the camera and stop the reader.\n";
            continue;
        }
        if (command == "status") {
            std::cout << "IDLE - camera open; waiting for the capture command.\n";
            continue;
        }
        if (command.empty()) {
            continue;
        }
        if (command != "capture") {
            std::cout << "Unknown command '" << command << "'. Type help for available commands.\n";
            continue;
        }

        ++captureNumber;
        const auto startedAt = std::chrono::steady_clock::now();
        gateDatabase.updateStatus("online", "active");
        std::cout << "CAPTURE " << captureNumber << ": acquiring a fresh camera frame...\n";

        // A camera that remains open while idle may have queued old frames.
        // Discard several buffers so inference always receives a current photo.
        cv::Mat frame;
        bool frameReady = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (camera.read(frame) && !frame.empty()) {
                frameReady = true;
            }
        }
        if (!frameReady) {
            std::cerr << "CAPTURE " << captureNumber << ": camera frame could not be read.\n";
            gateDatabase.updateStatus("online", "idle");
            continue;
        }
        const std::string captureStem = fs::path(
            eventSnapshotName("CAPTURE", captureNumber)
        ).stem().string();
        std::cout << "CAPTURE " << captureNumber
                  << ": frame held in memory only; running YOLO...\n";

        const std::vector<Detection> detections = detectPlates(detector, frame);
        if (detections.empty()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt
            ).count();
            std::cout << "CAPTURE " << captureNumber << ": NO PLATE DETECTED.\n";
            gateDatabase.updateStatus("online", "idle");
            std::cout << "CAPTURE " << captureNumber << ": complete in "
                      << elapsed << " ms; returning to IDLE.\n";
            continue;
        }

        struct CaptureResult {
            Detection detection;
            std::string plate;
            AuthorizationResult authorization;
            fs::path cropPath;
        };
        std::vector<CaptureResult> results;
        std::cout << "CAPTURE " << captureNumber << ": YOLO found "
                  << detections.size() << " plate region(s).\n";

        for (std::size_t index = 0; index < detections.size(); ++index) {
            const Detection& detection = detections[index];
            const cv::Mat crop = frame(detection.box).clone();
            const cv::Mat enhanced = zoomPlate(crop);
            const fs::path cropPath = cropDirectory /
                (captureStem + "-plate-" + std::to_string(index + 1) + ".jpg");
            cv::imwrite(cropPath.string(), enhanced, {cv::IMWRITE_JPEG_QUALITY, 95});
            std::cout << "CAPTURE " << captureNumber << ": OCR plate "
                      << index + 1 << " using " << cropPath.string() << "...\n";

            const std::string plate = readPlate(recognizer, enhanced);
            AuthorizationResult authorization;
            cv::Scalar color(0, 200, 255);
            std::string label = plate;
            if (plate != "UNREADABLE") {
                authorization = gateDatabase.authorize(plate);
                color = authorization.authorized
                    ? cv::Scalar(0, 210, 0)
                    : cv::Scalar(0, 0, 230);
                if (!authorization.ownerName.empty()) {
                    label += " " + authorization.ownerName;
                }
            }
            drawLabel(frame, detection.box, label, color);
            results.push_back({detection, plate, authorization, cropPath});
            std::cout << "CAPTURE " << captureNumber << ": OCR RESULT "
                      << plate << " (YOLO " << std::fixed << std::setprecision(2)
                      << detection.confidence << ")\n";
        }

        const fs::path temporaryLatest = outputDirectory / "latest-capture.tmp.jpg";
        if (cv::imwrite(temporaryLatest.string(), frame, {cv::IMWRITE_JPEG_QUALITY, 92})) {
            std::error_code error;
            fs::remove(latestCapturePath, error);
            error.clear();
            fs::rename(temporaryLatest, latestCapturePath, error);
            if (error) {
                std::cerr << "CAPTURE " << captureNumber
                          << ": unable to publish the annotated dashboard photo: "
                          << error.message() << '\n';
            } else {
                std::cout << "CAPTURE " << captureNumber
                          << ": dashboard preview updated "
                          << latestCapturePath.string() << '\n';
            }
        }

        for (const CaptureResult& result : results) {
            if (result.plate == "UNREADABLE") {
                std::cout << "UNREADABLE - plate region detected but OCR returned no value.\n";
                continue;
            }
            gateDatabase.updateStatus("online", "active", result.plate);
            const bool inserted = gateDatabase.recordEvent(
                result.plate,
                result.authorization,
                result.detection.confidence,
                result.cropPath.string()
            );
            if (inserted) {
                std::cout << (result.authorization.authorized ? "AUTHORIZED " : "DENIED ")
                          << result.plate;
                if (!result.authorization.ownerName.empty()) {
                    std::cout << ' ' << result.authorization.ownerName;
                }
                std::cout << '\n';
            } else {
                std::cout << "DUPLICATE SUPPRESSED " << result.plate << '\n';
            }
        }

        if (!headless) {
            cv::imshow("On-demand License Plate Recognition", frame);
            cv::waitKey(1);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt
        ).count();
        gateDatabase.updateStatus("online", "idle");
        std::cout << "CAPTURE " << captureNumber << ": complete in "
                  << elapsed << " ms; full frame discarded, returning to IDLE.\n";
    }

    camera.release();
    gateDatabase.updateStatus("offline", "stopped");
    if (!headless) {
        cv::destroyAllWindows();
    }
    return 0;
}
#endif

int main(int argc, char** argv) {
    const bool cameraMode = argc > 1 && std::string(argv[1]) == "--camera";
    const bool headless = std::find_if(
        argv + 1,
        argv + argc,
        [](const char* argument) { return std::string(argument) == "--headless"; }
    ) != argv + argc;
    const int cameraIndex = cameraMode && argc > 2 ? std::stoi(argv[2]) : 0;
    const fs::path inputDirectory = cameraMode
        ? "raw-images"
        : (argc > 1 ? argv[1] : "raw-images");
    const fs::path outputDirectory = cameraMode
        ? (argc > 5 ? argv[5] : "Output")
        : (argc > 2 ? argv[2] : "Output");
    const fs::path modelPath = argc > 3
        ? argv[3]
        : "models/license_plate_detector.onnx";
    const fs::path ocrModelPath = argc > 4
        ? argv[4]
        : "models/en_PP-OCRv5_rec_mobile.onnx";
    const fs::path databasePath = cameraMode && argc > 6
        ? argv[6]
        : "database/gate_access.db";
    fs::path commandFile;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--command-file") {
            commandFile = argv[index + 1];
            break;
        }
    }
    const fs::path cropDirectory = outputDirectory / "Plate-Crops";
    fs::create_directories(cropDirectory);

    cv::dnn::Net detector = cv::dnn::readNetFromONNX(modelPath.string());
    detector.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);

    cv::dnn::Net recognizer = cv::dnn::readNetFromONNX(ocrModelPath.string());
    recognizer.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);

    if (cameraMode) {
#ifdef PLATE_ENABLE_CAMERA
        return runCamera(
            detector,
            recognizer,
            cameraIndex,
            outputDirectory,
            databasePath,
            headless,
            commandFile
        );
#else
        std::cerr << "Camera support was disabled when this executable was built.\n";
        return 1;
#endif
    }

    std::vector<fs::path> inputs;
    for (const auto& entry : fs::directory_iterator(inputDirectory)) {
        if (entry.is_regular_file() && supportedImage(entry.path())) {
            inputs.push_back(entry.path());
        }
    }
    std::sort(inputs.begin(), inputs.end());

    for (const fs::path& path : inputs) {
        cv::Mat image = cv::imread(path.string(), cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cerr << path.filename().string() << ": unable to read image\n";
            continue;
        }

        const std::vector<Detection> detections = detectPlates(detector, image);
        std::vector<std::string> labels;
        for (std::size_t index = 0; index < detections.size(); ++index) {
            const cv::Mat crop = image(detections[index].box).clone();
            const cv::Mat zoomed = zoomPlate(crop);
            const fs::path cropPath = cropDirectory /
                (path.stem().string() + "_plate_" + std::to_string(index + 1) + ".jpg");
            cv::imwrite(cropPath.string(), zoomed, {cv::IMWRITE_JPEG_QUALITY, 95});

            const std::string text = readPlate(recognizer, zoomed);
            drawLabel(image, detections[index].box, text);
            labels.push_back(text);
        }

        const fs::path target = outputDirectory / path.filename();
        cv::imwrite(target.string(), image);
        std::cout << path.filename().string() << ": ";
        if (labels.empty()) {
            std::cout << "no plate detected";
        } else {
            for (std::size_t index = 0; index < labels.size(); ++index) {
                if (index > 0) std::cout << ", ";
                std::cout << labels[index];
            }
        }
        std::cout << '\n';
    }

    return 0;
}
