#include "gate_controller.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using gate::Milliseconds;
using gate::State;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct Fixture {
    gate::Config config;
    gate::Controller controller{config};
    gate::Inputs inputs;
    gate::TimePoint now{};

    gate::Snapshot update(long long advance = 0) {
        now += Milliseconds(advance);
        return controller.update(now, inputs);
    }

    void startRecognition() {
        inputs.loopPresent = true;
        update();
        update(config.inputDebounce.count());
        require(controller.state() == State::Recognizing, "loop starts recognition after debounce");
    }
};

void authorizedCycleAndWaitingVehicle() {
    Fixture fixture;
    require(fixture.update().state == State::IdleClosed, "healthy startup reaches idle closed");
    fixture.startRecognition();
    require(fixture.controller.recognitionCompleted(fixture.now, true), "authorization is accepted");
    auto status = fixture.update();
    require(status.state == State::Opening && status.outputs.requestOpen, "authorized cycle pulses open");
    require(status.outputs.greenLight && !status.outputs.redLight, "authorized movement shows green");

    fixture.inputs.fullyClosed = false;
    fixture.inputs.fullyOpen = true;
    require(fixture.update().state == State::GateOpen, "open limit confirms gate open");
    fixture.inputs.loopPresent = true;
    fixture.update(1000);
    require(fixture.controller.state() == State::GateOpen, "second loop activity cannot overlap cycle");

    fixture.inputs.passageBlocked = true;
    require(fixture.update().state == State::VehiclePassing, "passage blockage is detected");
    fixture.inputs.passageBlocked = false;
    require(fixture.update().state == State::ClearanceWait, "clear passage starts clearance timer");
    status = fixture.update(fixture.config.clearanceTime.count());
    require(status.state == State::Closing && status.outputs.requestClose, "clearance requests closing");
    require(status.outputs.redLight && !status.outputs.greenLight, "closing shows red");

    fixture.inputs.fullyOpen = false;
    fixture.inputs.fullyClosed = true;
    require(fixture.update().state == State::Rearming, "closed limit ends movement cycle");
    fixture.update();
    fixture.update(fixture.config.inputDebounce.count());
    require(fixture.controller.state() == State::Recognizing, "waiting vehicle starts after confirmed closure");
}

void denialRequiresLoopClear() {
    Fixture fixture;
    fixture.update();
    fixture.startRecognition();
    fixture.controller.recognitionCompleted(fixture.now, false);
    require(fixture.update().state == State::Denied, "denied plate keeps gate denied");
    fixture.update(5000);
    require(fixture.controller.state() == State::Denied, "occupied loop cannot retrigger denied vehicle");
    fixture.inputs.loopPresent = false;
    fixture.update();
    fixture.update(fixture.config.inputDebounce.count());
    require(fixture.controller.state() == State::IdleClosed, "denied cycle rearms only after loop clears");
}

void obstructionReopensAndBlocksCloseRelay() {
    Fixture fixture;
    fixture.update();
    fixture.startRecognition();
    fixture.controller.recognitionCompleted(fixture.now, true);
    fixture.inputs.fullyClosed = false;
    fixture.inputs.fullyOpen = true;
    fixture.update();
    fixture.inputs.passageBlocked = true;
    fixture.update();
    fixture.inputs.passageBlocked = false;
    fixture.update();
    fixture.update(fixture.config.clearanceTime.count());
    fixture.inputs.fullyOpen = false;
    fixture.inputs.passageBlocked = true;
    const auto status = fixture.update();
    require(status.state == State::Opening, "obstruction during closing requests reopen");
    require(status.outputs.requestOpen, "reopen relay pulses after obstruction");
    require(!status.outputs.requestClose, "close relay is off whenever passage is blocked");
}

void faultsAreFailSafe() {
    Fixture fixture;
    fixture.inputs.fullyOpen = true;
    const auto startupFault = fixture.update();
    require(startupFault.state == State::Fault, "contradictory limits fault at startup");
    require(startupFault.outputs.redLight && !startupFault.outputs.greenLight,
            "fault defaults to red light");
    require(!startupFault.outputs.requestOpen && !startupFault.outputs.requestClose,
            "fault removes movement requests");

    Fixture timeout;
    timeout.update();
    timeout.startRecognition();
    timeout.controller.recognitionCompleted(timeout.now, true);
    timeout.inputs.fullyClosed = false;
    timeout.update(timeout.config.openingTimeout.count());
    require(timeout.controller.state() == State::Fault, "opening timeout enters fault");
}

}  // namespace

int main() {
    authorizedCycleAndWaitingVehicle();
    denialRequiresLoopClear();
    obstructionReopensAndBlocksCloseRelay();
    faultsAreFailSafe();
    std::cout << "All gate-controller safety tests passed.\n";
    return 0;
}
