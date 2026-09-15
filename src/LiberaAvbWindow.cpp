#include "LiberaAvbWindow.h"
#include "AvbSettings.hpp"

#include "imgui.h"

#include <algorithm>

namespace libera::ui {

bool DrawAvbWindow(bool* open, AvbWindowState& state,
                   bool settingsLocked, bool discoveryEnabled) {
    if (!open || !*open) {
        state.wasOpen = false;
        return false;
    }

    bool discoveryChanged = false;
    auto saveSettings = [&]() {
        libera_link::saveAvbSettings(libera_link::avbSettingsPath(), state.lastError);
        state.lastRefresh = {};
    };

    ImGui::SetNextWindowSize(ImVec2(650.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("AVB Setup", open, ImGuiWindowFlags_NoCollapse)) {
        const auto now = std::chrono::steady_clock::now();
        // Refresh while the window is open to pick up connected interfaces,
        // without asking CoreAudio to enumerate devices on every UI frame.
        if (!state.wasOpen || now - state.lastRefresh >= std::chrono::seconds(1)) {
            state.devices = avb::AvbManager::availableDevices();
            state.controllers = avb::AvbManager::configuredControllers();
            state.lastRefresh = now;
        }
        state.wasOpen = true;

        ImGui::TextWrapped("Select the AVB audio interfaces to use as laser outputs. Each eight-channel bank appears as a controller in Libera Link.");
        ImGui::TextWrapped("All banks on an interface share its point rate.");
        if (!discoveryEnabled) {
            ImGui::TextWrapped("Enable AVB in Settings > Controller Discovery to discover the selected interfaces.");
        }
        if (settingsLocked) {
            ImGui::TextDisabled("Setup is paused during scans and while AVB controllers are linked.");
            ImGui::TextWrapped("Turn off the linked AVB controllers in the main window to change their setup.");
        }
        if (ImGui::Button("Refresh Interfaces")) {
            state.lastRefresh = {};
        }
        ImGui::Separator();

        if (state.devices.empty()) {
            ImGui::TextWrapped("No suitable audio interfaces found. Connect an AVB or audio interface with at least eight output channels and make it available to the operating system.");
        }

        const auto configs = avb::AvbManager::configuredDevices();
        ImGui::BeginDisabled(settingsLocked);
        for (const auto& device : state.devices) {
            ImGui::PushID(device.uid.c_str());
            const auto configIt = std::find_if(configs.begin(), configs.end(),
                [&](const auto& config) { return config.deviceUid == device.uid; });
            bool useDevice = configIt != configs.end();
            if (ImGui::Checkbox(device.label.c_str(), &useDevice)) {
                if (avb::AvbManager::setDeviceEnabled(device.uid, useDevice)) {
                    saveSettings();
                    discoveryChanged = true;
                } else {
                    state.lastError = "This interface is no longer available. Refresh the interface list.";
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%u channels, %u banks)", device.outputChannels, device.outputChannels / 8);

            if (useDevice) {
                ImGui::Indent();
                const auto pointRate = configIt != configs.end()
                    ? configIt->preferredPointRate : device.defaultPointRate;
                if (device.pointRateMutable && !device.supportedPointRates.empty()) {
                    const std::string preview = std::to_string(pointRate) + " pps";
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::BeginCombo("Point rate", preview.c_str())) {
                        for (const auto rate : device.supportedPointRates) {
                            const bool selected = rate == pointRate;
                            const std::string label = std::to_string(rate) + " pps";
                            if (ImGui::Selectable(label.c_str(), selected)) {
                                if (avb::AvbManager::setPreferredPointRate(device.uid, rate)) {
                                    saveSettings();
                                    discoveryChanged = true;
                                } else {
                                    state.lastError = "The point rate could not be changed. Refresh the interface list.";
                                }
                            }
                            if (selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    ImGui::Text("Point rate: %u pps", pointRate);
                }

                for (const auto& controller : state.controllers) {
                    if (controller.deviceUid() != device.uid) {
                        continue;
                    }
                    ImGui::PushID(controller.idValue().c_str());
                    ImGui::Text("Channels %u-%u", controller.channelOffset() + 1,
                                controller.channelOffset() + controller.channelCount());
                    bool halfXY = avb::AvbManager::halfXYOutputEnabled(controller.idValue());
                    if (ImGui::Checkbox("Half X/Y Output", &halfXY)) {
                        if (avb::AvbManager::setHalfXYOutput(controller.idValue(), halfXY)) {
                            saveSettings();
                        } else {
                            state.lastError = "The bank output setting could not be changed.";
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Halves the scan size for AVB-to-ILDA adapters that ground one side of each differential X/Y signal.");
                    }
                    ImGui::PopID();
                }
                ImGui::Unindent();
            }
            ImGui::Spacing();
            ImGui::PopID();
        }

        // Keep unplugged interfaces visible and removable without erasing
        // their saved configuration just because they are temporarily absent.
        for (const auto& config : configs) {
            const bool available = std::any_of(state.devices.begin(), state.devices.end(),
                [&](const auto& device) { return device.uid == config.deviceUid; });
            if (!available) {
                ImGui::PushID(config.deviceUid.c_str());
                ImGui::TextWrapped("Saved interface (disconnected): %s", config.deviceUid.c_str());
                if (ImGui::Button("Forget Interface")) {
                    avb::AvbManager::setDeviceEnabled(config.deviceUid, false);
                    saveSettings();
                    discoveryChanged = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndDisabled();

        if (!state.lastError.empty()) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", state.lastError.c_str());
            ImGui::PopStyleColor();
            if (ImGui::Button("Retry Save")) {
                saveSettings();
            }
        }
    }
    ImGui::End();
    return discoveryChanged;
}

} // namespace libera::ui
