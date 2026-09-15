#pragma once

#include "libera/avb/AvbManager.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace libera::ui {

struct AvbWindowState {
    bool wasOpen = false;
    std::chrono::steady_clock::time_point lastRefresh{};
    std::vector<avb::AvbAudioDeviceInfo> devices;
    std::vector<avb::AvbControllerInfo> controllers;
    std::string lastError;
};

// Returns true when interface selection or point rate changed and Link needs
// to rediscover controllers. Half X/Y changes take effect without a rescan.
bool DrawAvbWindow(bool* open, AvbWindowState& state,
                   bool settingsLocked, bool discoveryEnabled);

} // namespace libera::ui
