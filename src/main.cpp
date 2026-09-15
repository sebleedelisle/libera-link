#include "LinkRuntime.hpp"
#include "AvbSettings.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {

std::atomic<bool> gStopRequested{false};

void onSignal(int) {
    gStopRequested.store(true, std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    libera_link::LinkOptions options;
    const libera_link::ParseResult parseResult = libera_link::parseOptions(argc, argv, options);
    if (parseResult == libera_link::ParseResult::Help) {
        return 0;
    }
    if (parseResult != libera_link::ParseResult::Ok) {
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::string avbError;
    if (!libera_link::loadAvbSettings(libera_link::avbSettingsPath(), avbError)) {
        std::cerr << avbError << '\n';
    }
    libera_link::LinkRuntime runtime;
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
