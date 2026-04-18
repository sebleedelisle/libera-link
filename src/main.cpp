#include "BridgeRuntime.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace {

std::atomic<bool> gStopRequested{false};

void onSignal(int) {
    gStopRequested.store(true, std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    idn_bridge::BridgeOptions options;
    const idn_bridge::ParseResult parseResult = idn_bridge::parseOptions(argc, argv, options);
    if (parseResult == idn_bridge::ParseResult::Help) {
        return 0;
    }
    if (parseResult != idn_bridge::ParseResult::Ok) {
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    idn_bridge::BridgeRuntime runtime;
    runtime.setEchoLogsToStdStreams(true);
    if (!runtime.start(options)) {
        return 1;
    }

    while (!gStopRequested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    runtime.stop();
    return 0;
}
