#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace gate {

using Milliseconds = std::chrono::milliseconds;
using TimePoint = std::chrono::steady_clock::time_point;

enum class State {
    Startup,
    IdleClosed,
    Recognizing,
    Denied,
    Opening,
    GateOpen,
    VehiclePassing,
    ClearanceWait,
    Closing,
    Rearming,
    Fault
};

struct Config {
    Milliseconds inputDebounce{300};
    Milliseconds relayPulse{500};
    Milliseconds recognitionTimeout{20000};
    Milliseconds openingTimeout{12000};
    Milliseconds passageTimeout{30000};
    Milliseconds clearanceTime{1500};
    Milliseconds closingTimeout{12000};
};

struct Inputs {
    bool loopPresent = false;
    bool passageBlocked = false;
    bool fullyOpen = false;
    bool fullyClosed = true;
    bool barrierFault = false;
};

struct Outputs {
    bool requestOpen = false;
    bool requestClose = false;
    bool redLight = true;
    bool greenLight = false;
};

struct Snapshot {
    State state = State::Startup;
    Outputs outputs;
    bool cycleActive = false;
    std::string faultReason;
};

class Controller {
public:
    explicit Controller(Config config = {});

    Snapshot update(TimePoint now, const Inputs& inputs);
    bool recognitionCompleted(
        TimePoint now,
        bool authorized,
        bool systemError = false,
        const std::string& reason = ""
    );
    bool acknowledgeFault(TimePoint now, const Inputs& inputs);
    Snapshot snapshot(TimePoint now, const Inputs& inputs) const;

    State state() const;
    const std::string& faultReason() const;

private:
    Config config_;
    State state_ = State::Startup;
    TimePoint stateEnteredAt_{};
    TimePoint relayPulseEndsAt_{};
    bool started_ = false;
    std::string faultReason_;
    std::optional<TimePoint> loopOccupiedSince_;
    std::optional<TimePoint> loopClearSince_;

    void transition(State next, TimePoint now);
    void enterFault(const std::string& reason, TimePoint now);
    bool inputStableFor(
        bool level,
        TimePoint now,
        std::optional<TimePoint>& since,
        Milliseconds duration
    );
};

const char* stateName(State state);

}  // namespace gate
