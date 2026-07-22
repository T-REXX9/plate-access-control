#include "gate_controller.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void printSnapshot(const gate::Snapshot& snapshot, long long elapsedMs) {
    std::cout
        << "t=" << elapsedMs << "ms state=" << gate::stateName(snapshot.state)
        << " cycle=" << (snapshot.cycleActive ? "locked" : "free")
        << " open_relay=" << snapshot.outputs.requestOpen
        << " close_relay=" << snapshot.outputs.requestClose
        << " traffic=" << (snapshot.outputs.trafficGreen ? "GREEN" : "RED");
    if (!snapshot.faultReason.empty()) {
        std::cout << " fault=\"" << snapshot.faultReason << '"';
    }
    std::cout << '\n';
}

bool onValue(const std::string& value) {
    return value == "on" || value == "1" || value == "blocked" ||
        value == "occupied" || value == "active";
}

}  // namespace

int main() {
    gate::Config config;
    gate::Controller controller(config);
    gate::Inputs inputs;
    const gate::TimePoint beginning{};
    gate::TimePoint now = beginning;

    std::cout
        << "Gate simulator. Commands:\n"
        << "  loop on|off             passage blocked|clear\n"
        << "  authorize|deny|recognition-error\n"
        << "  tick MILLISECONDS       reset|status|quit\n";
    printSnapshot(controller.update(now, inputs), 0);

    std::string line;
    while (std::cout << "gate-sim> " && std::getline(std::cin, line)) {
        std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::istringstream command(line);
        std::string name;
        std::string value;
        command >> name >> value;

        if (name == "quit" || name == "exit") {
            break;
        } else if (name == "tick") {
            try {
                now += gate::Milliseconds(std::max(0LL, std::stoll(value)));
            } catch (const std::exception&) {
                std::cout << "tick requires a non-negative millisecond value.\n";
                continue;
            }
        } else if (name == "loop") {
            inputs.loopPresent = onValue(value);
        } else if (name == "passage") {
            inputs.passageBlocked = onValue(value);
        } else if (name == "authorize") {
            if (!controller.recognitionCompleted(now, true)) {
                std::cout << "authorize is valid only in RECOGNIZING.\n";
            }
        } else if (name == "deny") {
            if (!controller.recognitionCompleted(now, false)) {
                std::cout << "deny is valid only in RECOGNIZING.\n";
            }
        } else if (name == "recognition-error") {
            if (!controller.recognitionCompleted(now, false, true, "Simulated recognition failure")) {
                std::cout << "recognition-error is valid only in RECOGNIZING.\n";
            }
        } else if (name == "reset") {
            if (!controller.acknowledgeFault(now, inputs)) {
                std::cout << "Reset rejected: the IR passage input must be clear.\n";
            }
        } else if (name != "status") {
            std::cout << "Unknown command.\n";
            continue;
        }

        const auto snapshot = controller.update(now, inputs);
        printSnapshot(
            snapshot,
            std::chrono::duration_cast<gate::Milliseconds>(now - beginning).count()
        );
    }
    return 0;
}
