#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
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

cv::Mat allowedPlateInkMask(const cv::Mat& image) {
    cv::Mat hsv;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    cv::Mat dark;
    cv::Mat green;
    cv::Mat redLow;
    cv::Mat redHigh;
    cv::Mat yellow;
    cv::inRange(hsv, cv::Scalar(0, 0, 0), cv::Scalar(179, 255, 145), dark);
    cv::inRange(hsv, cv::Scalar(30, 38, 35), cv::Scalar(105, 255, 255), green);
    cv::inRange(hsv, cv::Scalar(0, 65, 45), cv::Scalar(13, 255, 255), redLow);
    cv::inRange(hsv, cv::Scalar(165, 65, 45), cv::Scalar(179, 255, 255), redHigh);
    cv::inRange(hsv, cv::Scalar(14, 55, 55), cv::Scalar(38, 255, 255), yellow);
    cv::Mat allowed = dark | green | redLow | redHigh | yellow;
    cv::morphologyEx(
        allowed,
        allowed,
        cv::MORPH_CLOSE,
        cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3))
    );
    return allowed;
}

cv::Mat isolateRegistrationCharacters(const cv::Mat& aligned, bool correctedSkew) {
    const cv::Mat supportedInk = allowedPlateInkMask(aligned);
    cv::Mat hsv;
    cv::Mat strictGreenMask;
    cv::cvtColor(aligned, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(40, 70, 40), cv::Scalar(90, 255, 255), strictGreenMask);
    const double topRatio = correctedSkew ? 0.10 : 0.05;
    const double strictGreenRatio = cv::countNonZero(strictGreenMask) /
        static_cast<double>(aligned.total());
    const double supportedInkRatio = cv::countNonZero(supportedInk) /
        static_cast<double>(aligned.total());
    const bool strongGreenRegistration = strictGreenRatio >= 0.10 &&
        supportedInkRatio >= 0.12;
    const double bottomRatio = strongGreenRegistration
        ? 0.80
        : (correctedSkew ? 0.80 : 0.85);
    const int top = static_cast<int>(aligned.rows * topRatio);
    const int bottom = std::max(top + 1, static_cast<int>(aligned.rows * bottomRatio));
    const cv::Rect bandBox(
        0,
        top,
        aligned.cols,
        std::min(bottom, aligned.rows) - top
    );

    // Color segmentation decides when the lower edge can be tightened to
    // discard a small same-color slogan. Preserve the natural antialiased
    // character edges because hard pixel masking reduces OCR accuracy.
    return aligned(bandBox).clone();
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

    // Use accepted character colors to locate the large registration row,
    // then reject smaller slogans, logos, borders, and stickers by component
    // size. Geometry-only cropping remains as a fallback for poor lighting.
    const cv::Mat registrationBand = isolateRegistrationCharacters(aligned, correctedSkew);

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

struct BurstCandidate {
    int frameIndex = 0;
    Detection detection;
    cv::Mat enhancedCrop;
    double sharpness = 0.0;
    double exposure = 0.0;
    double quality = 0.0;
};

struct OcrVote {
    std::string reading;
    double quality = 0.0;
    std::size_t candidateIndex = 0;
};

double plateSharpness(const cv::Mat& crop) {
    cv::Mat gray;
    cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
    cv::Mat laplacian;
    cv::Laplacian(gray, laplacian, CV_64F);
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(laplacian, mean, deviation);
    return deviation[0] * deviation[0];
}

double plateExposureScore(const cv::Mat& crop) {
    cv::Mat gray;
    cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
    const double brightness = cv::mean(gray)[0];
    const double brightnessScore = std::clamp(
        1.0 - std::abs(brightness - 135.0) / 135.0,
        0.0,
        1.0
    );
    cv::Mat shadows;
    cv::Mat highlights;
    cv::inRange(gray, cv::Scalar(0), cv::Scalar(15), shadows);
    cv::inRange(gray, cv::Scalar(240), cv::Scalar(255), highlights);
    const double clippedRatio = (
        cv::countNonZero(shadows) + cv::countNonZero(highlights)
    ) / static_cast<double>(gray.total());
    return brightnessScore * std::clamp(1.0 - clippedRatio * 2.0, 0.0, 1.0);
}

double plateCandidateQuality(
    const Detection& detection,
    const cv::Size& frameSize,
    double sharpness,
    double exposure
) {
    const double sharpnessScore = std::clamp(
        std::log1p(sharpness) / std::log(1001.0),
        0.0,
        1.0
    );
    const double areaRatio = detection.box.area() /
        static_cast<double>(frameSize.area());
    const double sizeScore = std::clamp(std::sqrt(areaRatio) / 0.20, 0.0, 1.0);
    return detection.confidence * 0.45 +
        sharpnessScore * 0.30 +
        exposure * 0.15 +
        sizeScore * 0.10;
}

int editDistance(const std::string& first, const std::string& second) {
    std::vector<int> previous(second.size() + 1);
    std::vector<int> current(second.size() + 1);
    for (std::size_t index = 0; index <= second.size(); ++index) {
        previous[index] = static_cast<int>(index);
    }
    for (std::size_t row = 1; row <= first.size(); ++row) {
        current[0] = static_cast<int>(row);
        for (std::size_t column = 1; column <= second.size(); ++column) {
            const int substitution = previous[column - 1] +
                (first[row - 1] == second[column - 1] ? 0 : 1);
            current[column] = std::min({
                previous[column] + 1,
                current[column - 1] + 1,
                substitution
            });
        }
        std::swap(previous, current);
    }
    return previous.back();
}

std::string consensusPlate(const std::vector<OcrVote>& votes) {
    std::vector<OcrVote> readable;
    for (const OcrVote& vote : votes) {
        if (!vote.reading.empty() && vote.reading != "UNREADABLE") {
            readable.push_back(vote);
        }
    }
    if (readable.empty()) {
        return "UNREADABLE";
    }
    if (readable.size() == 1) {
        return readable.front().reading;
    }

    struct VoteTotal {
        int count = 0;
        double weight = 0.0;
    };
    std::map<std::string, VoteTotal> exactVotes;
    for (const OcrVote& vote : readable) {
        VoteTotal& total = exactVotes[vote.reading];
        ++total.count;
        total.weight += vote.quality;
    }
    std::string exactWinner;
    VoteTotal exactWinnerTotal;
    for (const auto& [reading, total] : exactVotes) {
        if (total.count > exactWinnerTotal.count ||
            (total.count == exactWinnerTotal.count && total.weight > exactWinnerTotal.weight)) {
            exactWinner = reading;
            exactWinnerTotal = total;
        }
    }
    if (exactWinnerTotal.count >= 2) {
        return exactWinner;
    }

    std::map<std::size_t, VoteTotal> lengthVotes;
    for (const OcrVote& vote : readable) {
        VoteTotal& total = lengthVotes[vote.reading.size()];
        ++total.count;
        total.weight += vote.quality;
    }
    std::size_t consensusLength = 0;
    VoteTotal lengthWinnerTotal;
    for (const auto& [length, total] : lengthVotes) {
        if (total.count > lengthWinnerTotal.count ||
            (total.count == lengthWinnerTotal.count && total.weight > lengthWinnerTotal.weight)) {
            consensusLength = length;
            lengthWinnerTotal = total;
        }
    }
    if (lengthWinnerTotal.count >= 2) {
        std::string result;
        result.reserve(consensusLength);
        for (std::size_t position = 0; position < consensusLength; ++position) {
            std::map<char, double> characterVotes;
            for (const OcrVote& vote : readable) {
                if (vote.reading.size() == consensusLength) {
                    characterVotes[vote.reading[position]] += std::max(0.01, vote.quality);
                }
            }
            const auto winner = std::max_element(
                characterVotes.begin(),
                characterVotes.end(),
                [](const auto& first, const auto& second) {
                    return first.second < second.second;
                }
            );
            result.push_back(winner->first);
        }
        return result;
    }

    // When all readings have different lengths, choose the medoid: the OCR
    // value with the smallest total edit distance to the other observations.
    std::string medoid = readable.front().reading;
    int bestDistance = std::numeric_limits<int>::max();
    double bestQuality = -1.0;
    for (const OcrVote& candidate : readable) {
        int distance = 0;
        for (const OcrVote& other : readable) {
            distance += editDistance(candidate.reading, other.reading);
        }
        if (distance < bestDistance ||
            (distance == bestDistance && candidate.quality > bestQuality)) {
            medoid = candidate.reading;
            bestDistance = distance;
            bestQuality = candidate.quality;
        }
    }
    return medoid;
}

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
    const fs::path latestCapturePath = outputDirectory / "latest-plate-crop.jpg";
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
                << "capture  Grab five fresh frames and run quality-ranked YOLO/OCR consensus.\n"
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
        constexpr int burstFrameCount = 5;
        constexpr int ocrCandidateCount = 3;
        std::cout << "CAPTURE " << captureNumber << ": acquiring "
                  << burstFrameCount << " fresh camera frames...\n";

        // A camera that remains open while idle may have queued old frames.
        // Flush those buffers, then retain only a short in-memory burst.
        cv::Mat discardedFrame;
        for (int attempt = 0; attempt < 6; ++attempt) {
            camera.read(discardedFrame);
        }
        std::vector<cv::Mat> frames;
        frames.reserve(burstFrameCount);
        for (int index = 0; index < burstFrameCount; ++index) {
            cv::Mat frame;
            if (camera.read(frame) && !frame.empty()) {
                frames.push_back(frame.clone());
            }
        }
        if (frames.empty()) {
            std::cerr << "CAPTURE " << captureNumber
                      << ": camera burst could not be read.\n";
            gateDatabase.updateStatus("online", "idle");
            continue;
        }
        std::cout << "CAPTURE " << captureNumber << ": captured "
                  << frames.size() << '/' << burstFrameCount
                  << " frames; running YOLO on the burst...\n";
        const std::string captureStem = fs::path(
            eventSnapshotName("CAPTURE", captureNumber)
        ).stem().string();

        std::vector<BurstCandidate> candidates;
        for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
            const cv::Mat& frame = frames[frameIndex];
            const std::vector<Detection> detections = detectPlates(detector, frame);
            std::cout << "CAPTURE " << captureNumber << ": frame "
                      << frameIndex + 1 << '/' << frames.size() << " - YOLO found "
                      << detections.size() << " plate region(s).\n";

            bool foundFrameCandidate = false;
            BurstCandidate bestFrameCandidate;
            for (const Detection& detection : detections) {
                const cv::Mat crop = frame(detection.box).clone();
                const double sharpness = plateSharpness(crop);
                const double exposure = plateExposureScore(crop);
                const double quality = plateCandidateQuality(
                    detection,
                    frame.size(),
                    sharpness,
                    exposure
                );
                if (!foundFrameCandidate || quality > bestFrameCandidate.quality) {
                    bestFrameCandidate = {
                        static_cast<int>(frameIndex),
                        detection,
                        zoomPlate(crop),
                        sharpness,
                        exposure,
                        quality
                    };
                    foundFrameCandidate = true;
                }
            }
            if (foundFrameCandidate) {
                candidates.push_back(std::move(bestFrameCandidate));
            }
        }

        if (candidates.empty()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt
            ).count();
            std::cout << "CAPTURE " << captureNumber
                      << ": NO PLATE DETECTED IN ANY BURST FRAME.\n";
            gateDatabase.updateStatus("online", "idle");
            std::cout << "CAPTURE " << captureNumber << ": complete in "
                      << elapsed << " ms; returning to IDLE.\n";
            continue;
        }

        std::sort(candidates.begin(), candidates.end(), [](
            const BurstCandidate& first,
            const BurstCandidate& second
        ) {
            return first.quality > second.quality;
        });
        if (candidates.size() > ocrCandidateCount) {
            candidates.resize(ocrCandidateCount);
        }

        std::vector<OcrVote> votes;
        votes.reserve(candidates.size());
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const BurstCandidate& candidate = candidates[index];
            const std::string reading = readPlate(recognizer, candidate.enhancedCrop);
            votes.push_back({reading, candidate.quality, index});
            std::cout << "CAPTURE " << captureNumber << ": OCR sample "
                      << index + 1 << '/' << candidates.size()
                      << " from frame " << candidate.frameIndex + 1
                      << " = " << reading
                      << " (quality " << std::fixed << std::setprecision(2)
                      << candidate.quality << ", sharpness "
                      << std::setprecision(0) << candidate.sharpness << ")\n";
        }

        const std::string plate = consensusPlate(votes);
        std::size_t winnerIndex = 0;
        int winnerDistance = std::numeric_limits<int>::max();
        double winnerQuality = -1.0;
        for (const OcrVote& vote : votes) {
            const int distance = plate == "UNREADABLE" || vote.reading == "UNREADABLE"
                ? std::numeric_limits<int>::max() / 2
                : editDistance(vote.reading, plate);
            if (distance < winnerDistance ||
                (distance == winnerDistance && vote.quality > winnerQuality)) {
                winnerIndex = vote.candidateIndex;
                winnerDistance = distance;
                winnerQuality = vote.quality;
            }
        }
        const BurstCandidate& winner = candidates[winnerIndex];
        cv::Mat annotatedFrame = frames[winner.frameIndex].clone();
        const fs::path cropPath = cropDirectory / (captureStem + "-plate.jpg");
        cv::imwrite(
            cropPath.string(),
            winner.enhancedCrop,
            {cv::IMWRITE_JPEG_QUALITY, 95}
        );

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
        drawLabel(annotatedFrame, winner.detection.box, label, color);
        std::cout << "CAPTURE " << captureNumber << ": CONSENSUS " << plate
                  << " from " << votes.size() << " OCR sample(s).\n";

        const fs::path temporaryLatest = outputDirectory / "latest-plate-crop.tmp.jpg";
        if (cv::imwrite(
                temporaryLatest.string(),
                winner.enhancedCrop,
                {cv::IMWRITE_JPEG_QUALITY, 95}
            )) {
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
                          << ": dashboard crop preview updated "
                          << latestCapturePath.string() << '\n';
            }
        }

        if (plate == "UNREADABLE") {
            std::cout << "UNREADABLE - plate regions were detected but the OCR burst "
                      << "returned no value.\n";
        } else {
            gateDatabase.updateStatus("online", "active", plate);
            const bool inserted = gateDatabase.recordEvent(
                plate,
                authorization,
                winner.detection.confidence,
                cropPath.string()
            );
            if (inserted) {
                std::cout << (authorization.authorized ? "AUTHORIZED " : "DENIED ")
                          << plate;
                if (!authorization.ownerName.empty()) {
                    std::cout << ' ' << authorization.ownerName;
                }
                std::cout << '\n';
            } else {
                std::cout << "DUPLICATE SUPPRESSED " << plate << '\n';
            }
        }

        if (!headless) {
            cv::imshow("On-demand License Plate Recognition", annotatedFrame);
            cv::waitKey(1);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt
        ).count();
        gateDatabase.updateStatus("online", "idle");
        std::cout << "CAPTURE " << captureNumber << ": complete in "
                  << elapsed << " ms; burst frames discarded, returning to IDLE.\n";
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
