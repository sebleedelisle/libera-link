#include "AvbSettings.hpp"
#include "LinkRuntime.hpp"
#include "virtual_controller/VirtualControllerHostRegistry.hpp"

#include "libera/avb/AvbManager.hpp"
#include "../extern/libera-laser/src/avb/AvbBackendState.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {

namespace avb = libera::avb;
namespace vc = libera_link::virtual_controller;
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

class FakeOutputStream : public avb::detail::AudioOutputStream {
public:
    explicit FakeOutputStream(std::uint32_t rateValue) : rate(rateValue) {}
    bool start() override { return true; }
    void stop() override {}
    std::uint32_t pointRate() const override { return rate; }
    std::uint32_t channelCount() const override { return 16; }
private:
    std::uint32_t rate;
};

class FakeAudioHost : public avb::detail::AudioHost {
public:
    std::vector<avb::detail::AudioOutputDeviceInfo> devices{
        {1, "avb-test", "Test AVB interface", 16, 48000, true, {48000, 96000}}
    };
    int openCount = 0;
    avb::detail::AudioOutputCallback render;

    std::vector<avb::detail::AudioOutputDeviceInfo> listOutputDevices() override {
        return devices;
    }
    std::unique_ptr<avb::detail::AudioOutputStream> openOutputStream(
        const avb::detail::AudioOutputDeviceInfo&, std::uint32_t rate,
        avb::detail::AudioOutputCallback callback) override {
        ++openCount;
        render = std::move(callback);
        return std::make_unique<FakeOutputStream>(rate);
    }
};

// Capture the actual Link targets through its public virtual-host interface.
// This exercises Link's bridge without connecting to real lasers or sockets.
struct RecordingHostState {
    std::vector<vc::Target> targets;
};

class RecordingHost : public vc::VirtualControllerHost {
public:
    explicit RecordingHost(std::shared_ptr<RecordingHostState> stateValue)
        : state(std::move(stateValue)) {}
    std::string_view name() const override { return "avb-test-host"; }
    std::string_view displayName() const override { return "AVB test host"; }
    bool start(const vc::VirtualControllerHostContext& context, std::string&) override {
        state->targets = context.targets;
        active = true;
        return true;
    }
    void stop() override {
        state->targets.clear();
        active = false;
    }
    bool running() const override { return active; }
    std::vector<vc::VirtualControllerEndpoint> endpoints() const override { return {}; }
private:
    std::shared_ptr<RecordingHostState> state;
    bool active = false;
};

void testSavedSettings(const std::shared_ptr<FakeAudioHost>& host,
                       const std::filesystem::path& directory) {
    const auto path = directory / "avb-settings.txt";
    std::string error;
    check(libera_link::loadAvbSettings(path, error), "first run accepts missing settings");
    const std::string unpluggedId = "Disconnected \\\"interface\\\" \\ path";
    avb::AvbManager::setConfiguredDevices({{"avb-test", 96000}, {unpluggedId, 48000}});
    avb::AvbManager::setHalfXYOutputControllers({"avb-test::ch-0"});
    check(libera_link::saveAvbSettings(path, error), "settings save succeeds");
    avb::AvbManager::setConfiguredDevices({});
    avb::AvbManager::setHalfXYOutputControllers({});
    check(libera_link::loadAvbSettings(path, error), "settings restore succeeds");
    const auto configs = avb::AvbManager::configuredDevices();
    check(configs.size() == 2, "restore retains disconnected interfaces");
    check(std::any_of(configs.begin(), configs.end(), [&](const auto& config) {
        return config.deviceUid == unpluggedId;
    }), "quoted interface IDs survive save and restore");
    check(avb::AvbManager::halfXYOutputEnabled("avb-test::ch-0"), "per-bank Half X/Y restores");
    check(host->openCount == 0, "restoring setup does not start an output stream");

    {
        std::ofstream damaged(path, std::ios::trunc);
        damaged << "device \"avb-test\" 48000\ninvalid setting\n";
    }
    check(!libera_link::loadAvbSettings(path, error), "malformed settings report an error");
    const auto afterFailure = avb::AvbManager::configuredDevices();
    check(std::any_of(afterFailure.begin(), afterFailure.end(), [](const auto& config) {
        return config.deviceUid == "avb-test" && config.preferredPointRate == 96000;
    }), "a partial read does not replace the previous setup");
    check(!libera_link::saveAvbSettings(directory / "missing" / "settings.txt", error),
          "unwritable settings report an error");
}

void testLinkDiscoveryAndOutput(const std::shared_ptr<FakeAudioHost>& host) {
    auto state = std::make_shared<RecordingHostState>();
    vc::VirtualControllerHostRegistration registration;
    registration.info.id = "avb-test-host";
    registration.info.displayName = "AVB test host";
    registration.factory = [state](const auto&) { return std::make_unique<RecordingHost>(state); };
    check(vc::registerVirtualControllerHost(std::move(registration)), "test virtual host registers");

    avb::AvbManager::setConfiguredDevices({{"avb-test", 96000}});
    avb::AvbManager::setHalfXYOutputControllers({"avb-test::ch-0"});
    libera::System::setPluginDirectory("");
    libera_link::LinkOptions options;
    for (const auto& manager : libera::System::availableControllerManagers()) {
        if (manager.type != "AVB") {
            options.disabledControllerTypes.insert(manager.type);
        }
    }
    options.virtualControllerHostId = "avb-test-host";
    options.discoveryTimeoutMs = 50;
    {
        libera_link::LinkRuntime runtime;
        check(runtime.scan(options), "Link can scan the AVB backend");
        const auto discovery = runtime.snapshot();
        check(discovery.discovered.size() == 2, "16 channels appear as two linkable banks");
        check(std::all_of(discovery.discovered.begin(), discovery.discovered.end(), [](const auto& controller) {
            return controller.type == "AVB" && controller.linkable;
        }), "AVB banks are linkable controllers");
        check(host->openCount == 0, "discovery does not open the audio stream");
        check(runtime.start(options, {"avb-test::ch-0", "avb-test::ch-8"}), "AVB banks start through Link");
        const auto started = runtime.snapshot();
        check(std::any_of(started.recentLogs.begin(), started.recentLogs.end(), [](const auto& line) {
            return line.find("from the latest scan") != std::string::npos;
        }), "starting known controllers reuses the latest scan");
        check(state->targets.size() == 2, "virtual host receives both AVB targets");
        check(host->openCount == 1, "sibling banks share one audio stream");
        for (const auto& target : state->targets) {
            check(target.sink->status().outputPointRate == 96000,
                  "Link initially reports the configured AVB point rate");
            vc::SliceSubmission slice;
            slice.points.resize(512);
            for (auto& point : slice.points) {
                point.x = 0.8f;
                point.y = -0.6f;
                point.r = 0.7f;
                point.i = 1.0f;
            }
            // A sender's local rate command must not change the shared audio
            // clock or make Link misreport its hardware output/buffering rate.
            slice.durationUs = 10667;
            slice.effectivePointRate = 48000;
            vc::FrameSubmission frame;
            frame.slices.push_back(std::move(slice));
            check(target.sink->replaceFrame(std::move(frame)).accepted, "AVB target accepts a frame");
            check(target.sink->status().outputPointRate == 96000,
                  "sender rate changes preserve the actual shared AVB output rate");
        }
        if (host->render && state->targets.size() == 2) {
            std::vector<float> samples(256 * 16);
            host->render(samples.data(), 256, 16, {});
            check(std::fabs(samples[0] - 0.4f) < 0.001f, "first bank halves X output");
            check(std::fabs(samples[1] + 0.3f) < 0.001f, "first bank halves Y output");
            // Libera blanks RGB for the first millisecond after arming (96
            // points here). Validate colour after that startup blanking ends.
            check(samples[2] == 0.0f, "AVB preserves startup colour blanking");
            check(std::fabs(samples[240 * 16 + 2] - 0.7f) < 0.001f,
                  "Half X/Y leaves colour intact after startup blanking");
            check(std::fabs(samples[8] - 0.8f) < 0.001f, "second bank retains full X output");
            check(std::fabs(samples[9] + 0.6f) < 0.001f, "second bank retains full Y output");
        }
        runtime.stop();
    }
}

} // namespace

int main() {
    auto host = std::make_shared<FakeAudioHost>();
    avb::detail::AvbBackendState::setAudioHostFactoryForTesting([host] { return host; });
    const auto directory = std::filesystem::temp_directory_path() /
        ("libera-link-avb-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    testSavedSettings(host, directory);
    testLinkDiscoveryAndOutput(host);
    avb::detail::AvbBackendState::setAudioHostFactoryForTesting({});
    std::filesystem::remove_all(directory);
    std::fprintf(stderr, "test_avb_link: %s\n", failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}
