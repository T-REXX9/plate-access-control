#include "gate_controller.hpp"

#include <utility>

namespace gate {

Controller::Controller(Config config) : config_(std::move(config)) {}

void Controller::transition(State next, TimePoint now) {
    state_ = next;
    stateEnteredAt_ = now;
    loopOccupiedSince_.reset();
    loopClearSince_.reset();
    if (next == State::Opening || next == State::Closing) {
        relayPulseEndsAt_ = now + config_.relayPulse;
    }
}

void Controller::enterFault(const std::string& reason, TimePoint now) {
    faultReason_ = reason;
    transition(State::Fault, now);
}

bool Controller::inputStableFor(
    bool level,
    TimePoint now,
    std::optional<TimePoint>& since,
    Milliseconds duration
) {
    if (!level) {
        since.reset();
        return false;
    }
    if (!since) {
        since = now;
    }
    return now - *since >= duration;
}

Snapshot Controller::update(TimePoint now, const Inputs& inputs) {
    if (!started_) {
        started_ = true;
        stateEnteredAt_ = now;
    }

    if (state_ != State::Fault) {
        if (inputs.fullyOpen && inputs.fullyClosed) {
            enterFault("Open and closed limit inputs are active together", now);
        } else if (inputs.barrierFault) {
            enterFault("Barrier controller fault input is active", now);
        }
    }

    switch (state_) {
        case State::Startup:
            if (inputs.passageBlocked) {
                enterFault("Passage safety sensor is blocked at startup", now);
            } else if (!inputs.fullyClosed || inputs.fullyOpen) {
                enterFault("Barrier position is not confirmed closed at startup", now);
            } else {
                faultReason_.clear();
                transition(State::IdleClosed, now);
            }
            break;

        case State::IdleClosed:
            if (!inputs.fullyClosed) {
                enterFault("Closed limit was lost while idle", now);
            } else if (inputStableFor(
                inputs.loopPresent,
                now,
                loopOccupiedSince_,
                config_.inputDebounce
            )) {
                transition(State::Recognizing, now);
            }
            break;

        case State::Recognizing:
            if (!inputs.fullyClosed) {
                enterFault("Closed limit was lost during recognition", now);
            } else if (now - stateEnteredAt_ >= config_.recognitionTimeout) {
                enterFault("Plate recognition timed out", now);
            }
            break;

        case State::Denied:
            if (!inputs.fullyClosed) {
                enterFault("Closed limit was lost while access was denied", now);
            } else if (inputStableFor(
                !inputs.loopPresent,
                now,
                loopClearSince_,
                config_.inputDebounce
            )) {
                transition(State::IdleClosed, now);
            }
            break;

        case State::Opening:
            if (inputs.fullyOpen) {
                transition(State::GateOpen, now);
            } else if (now - stateEnteredAt_ >= config_.openingTimeout) {
                enterFault("Barrier did not reach the open limit", now);
            }
            break;

        case State::GateOpen:
            if (!inputs.fullyOpen) {
                enterFault("Open limit was lost while waiting for passage", now);
            } else if (inputs.passageBlocked) {
                transition(State::VehiclePassing, now);
            } else if (now - stateEnteredAt_ >= config_.passageTimeout) {
                enterFault("No passage was detected while the barrier was open", now);
            }
            break;

        case State::VehiclePassing:
            if (!inputs.fullyOpen) {
                enterFault("Open limit was lost while a vehicle was passing", now);
            } else if (!inputs.passageBlocked) {
                transition(State::ClearanceWait, now);
            } else if (now - stateEnteredAt_ >= config_.passageTimeout) {
                enterFault("Passage sensor remained blocked too long", now);
            }
            break;

        case State::ClearanceWait:
            if (!inputs.fullyOpen) {
                enterFault("Open limit was lost during passage clearance", now);
            } else if (inputs.passageBlocked) {
                transition(State::VehiclePassing, now);
            } else if (now - stateEnteredAt_ >= config_.clearanceTime) {
                transition(State::Closing, now);
            }
            break;

        case State::Closing:
            if (inputs.passageBlocked) {
                transition(State::Opening, now);
            } else if (inputs.fullyClosed) {
                transition(State::Rearming, now);
            } else if (now - stateEnteredAt_ >= config_.closingTimeout) {
                enterFault("Barrier did not reach the closed limit", now);
            }
            break;

        case State::Rearming:
            if (!inputs.fullyClosed) {
                enterFault("Closed limit was lost while rearming", now);
            } else if (inputStableFor(
                inputs.loopPresent,
                now,
                loopOccupiedSince_,
                config_.inputDebounce
            )) {
                transition(State::Recognizing, now);
            } else if (inputStableFor(
                !inputs.loopPresent,
                now,
                loopClearSince_,
                config_.inputDebounce
            )) {
                transition(State::IdleClosed, now);
            }
            break;

        case State::Fault:
            break;
    }

    return snapshot(now, inputs);
}

bool Controller::recognitionCompleted(
    TimePoint now,
    bool authorized,
    bool systemError,
    const std::string& reason
) {
    if (state_ != State::Recognizing) {
        return false;
    }
    if (systemError) {
        enterFault(reason.empty() ? "Plate recognition system failed" : reason, now);
    } else if (authorized) {
        transition(State::Opening, now);
    } else {
        transition(State::Denied, now);
    }
    return true;
}

bool Controller::acknowledgeFault(TimePoint now, const Inputs& inputs) {
    if (state_ != State::Fault || inputs.barrierFault || inputs.passageBlocked ||
        inputs.fullyOpen || !inputs.fullyClosed) {
        return false;
    }
    faultReason_.clear();
    transition(State::IdleClosed, now);
    return true;
}

Snapshot Controller::snapshot(TimePoint now, const Inputs& inputs) const {
    Outputs outputs;
    const bool authorizedMovement = state_ == State::Opening ||
        state_ == State::GateOpen || state_ == State::VehiclePassing ||
        state_ == State::ClearanceWait;
    outputs.redLight = !authorizedMovement;
    outputs.greenLight = authorizedMovement;
    outputs.requestOpen = state_ == State::Opening && now < relayPulseEndsAt_;
    outputs.requestClose = state_ == State::Closing && now < relayPulseEndsAt_ &&
        !inputs.passageBlocked;

    const bool cycleActive = state_ != State::Startup &&
        state_ != State::IdleClosed && state_ != State::Fault;
    return {state_, outputs, cycleActive, faultReason_};
}

State Controller::state() const {
    return state_;
}

const std::string& Controller::faultReason() const {
    return faultReason_;
}

const char* stateName(State state) {
    switch (state) {
        case State::Startup: return "STARTUP";
        case State::IdleClosed: return "IDLE_CLOSED";
        case State::Recognizing: return "RECOGNIZING";
        case State::Denied: return "DENIED";
        case State::Opening: return "OPENING";
        case State::GateOpen: return "GATE_OPEN";
        case State::VehiclePassing: return "VEHICLE_PASSING";
        case State::ClearanceWait: return "CLEARANCE_WAIT";
        case State::Closing: return "CLOSING";
        case State::Rearming: return "REARMING";
        case State::Fault: return "FAULT";
    }
    return "UNKNOWN";
}

}  // namespace gate
