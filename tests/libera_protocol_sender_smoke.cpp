#include "libera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace {

libera::core::Frame makeFrame(float phase) {
    libera::core::Frame frame;
    constexpr std::size_t points = 360;
    constexpr float brightness = 0.18f;
    const float tau = 2.0f * static_cast<float>(std::acos(-1.0));
    frame.points.reserve(points);

    for (std::size_t i = 0; i < points; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(points);
        const float angle = (t * tau) + phase;
        const float radius = 0.55f + (0.15f * std::sin((phase * 2.0f) + (t * tau * 3.0f)));
        libera::core::LaserPoint point;
        point.x = std::cos(angle) * radius;
        point.y = std::sin(angle) * radius;
        point.r = brightness * (0.5f + 0.5f * std::sin(phase + t * tau));
        point.g = brightness * (0.5f + 0.5f * std::sin(phase + t * tau + (tau / 3.0f)));
        point.b = brightness * (0.5f + 0.5f * std::sin(phase + t * tau + ((2.0f * tau) / 3.0f)));
        point.i = std::max({point.r, point.g, point.b});
        frame.points.push_back(point);
    }

    return frame;
}

} // namespace

int main(int argc, char** argv) {
    int frameCount = 120;
    if (argc > 1) {
        frameCount = std::max(1, std::atoi(argv[1]));
    }

    libera::SystemOptions options;
    options.disabledControllerTypes = {
        "EtherDream",
        "Helios",
        "IDN",
        "LaserCubeNet",
        "LaserCubeUSB",
        "AVB",
    };

    libera::System system(options);
    std::vector<std::unique_ptr<libera::core::ControllerInfo>> discovered;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        discovered = system.discoverControllers();
        if (!discovered.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (discovered.empty()) {
        std::cerr << "No Libera Protocol receivers discovered.\n";
        return 1;
    }

    const auto& info = *discovered.front();
    std::cout << "Connecting to " << info.labelValue()
              << " [" << info.type() << ":" << info.idValue() << "]";
    if (info.networkInfo()) {
        std::cout << " at " << info.networkInfo()->ip << ':' << info.networkInfo()->port;
    }
    std::cout << '\n';

    auto controller = system.connectController(info);
    if (!controller) {
        std::cerr << "Failed to connect to Libera Protocol receiver.\n";
        return 1;
    }

    libera::core::LaserController::setTargetLatency(std::chrono::milliseconds(50));
    controller->setPointRate(30000);
    controller->setArmed(true);

    float phase = 0.0f;
    int sent = 0;
    for (; sent < frameCount; ++sent) {
        while (!controller->isReadyForNewFrame()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!controller->sendFrame(makeFrame(phase))) {
            std::cerr << "sendFrame failed at frame " << sent << '\n';
            break;
        }
        phase += 0.08f;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    controller->setArmed(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    system.shutdown();

    std::cout << "Sent " << sent << " frame(s).\n";
    return sent == frameCount ? 0 : 1;
}
