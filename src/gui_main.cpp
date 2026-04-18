#include "BridgeRuntime.hpp"
#include "LiberaApp.h"
#include "LiberaPaths.hpp"
#include "LiberaPluginsWindow.h"
#include "LiberaWidgets.h"

#include "fonts/IconsForkAwesome.h"
#include "imgui.h"

#include <cfloat>
#include <chrono>
#include <future>
#include <set>
#include <utility>
#include <string>

namespace {

using namespace std::chrono_literals;

ImVec4 statusColor(idn_bridge::RuntimeState state) {
    switch (state) {
    case idn_bridge::RuntimeState::Running:
        return ImVec4(0.40f, 0.88f, 0.55f, 1.0f);
    case idn_bridge::RuntimeState::Scanning:
    case idn_bridge::RuntimeState::Starting:
    case idn_bridge::RuntimeState::StopRequested:
        return ImVec4(0.95f, 0.78f, 0.32f, 1.0f);
    case idn_bridge::RuntimeState::Failed:
        return ImVec4(0.95f, 0.34f, 0.34f, 1.0f);
    case idn_bridge::RuntimeState::Stopped:
    default:
        return ImVec4(0.68f, 0.72f, 0.78f, 1.0f);
    }
}

float drawBrandLogo(const LiberaApp& app, ImVec2 pos, float areaWidth, bool rightAlign = false) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImFont* boldFont = app.fontLarge ? app.fontLarge : ImGui::GetFont();
    ImFont* defaultFont = app.fontBase ? app.fontBase : ImGui::GetFont();

    const float logoFontSize = boldFont->FontSize * io.FontGlobalScale;
    const float baseSubFontSize = defaultFont->FontSize * io.FontGlobalScale * 0.85f;
    const char* firstWord = "LIBERA";
    const char* secondWord = "PORT";
    const char* subtitle = "UNIVERSAL TRANSLATOR FOR LASERS";

    const ImVec2 firstWordSize = boldFont->CalcTextSizeA(logoFontSize, FLT_MAX, 0.0f, firstWord);
    const ImVec2 secondWordSize = boldFont->CalcTextSizeA(logoFontSize, FLT_MAX, 0.0f, secondWord);
    const float logoGap = 4.0f;
    const float logoWidth = firstWordSize.x + logoGap + secondWordSize.x;

    const float subtitleFontSize = baseSubFontSize;
    const ImVec2 subtitleSize = defaultFont->CalcTextSizeA(subtitleFontSize, FLT_MAX, 0.0f, subtitle);

    const float y = pos.y;
    const float logoX = rightAlign ? pos.x + areaWidth - logoWidth : pos.x;
    drawList->AddText(boldFont, logoFontSize, ImVec2(logoX, y),
                      IM_COL32(220, 50, 220, 255), firstWord);
    drawList->AddText(boldFont, logoFontSize, ImVec2(logoX + firstWordSize.x + logoGap, y),
                      IM_COL32(153, 153, 153, 255), secondWord);

    const float subtitleX = logoX;
    const float subtitleY = y + firstWordSize.y + 1.0f;
    drawList->AddText(defaultFont, subtitleFontSize, ImVec2(subtitleX, subtitleY),
                      IM_COL32(100, 100, 100, 200), subtitle);

    return firstWordSize.y + 1.0f + subtitleSize.y;
}
} // namespace

int main() {
    LiberaApp app;
    if (!app.init({"Lib Port", 1180, 780, -1, -1})) {
        return 1;
    }

    idn_bridge::BridgeRuntime runtime;

    std::future<bool> scanFuture;
    std::future<bool> startFuture;
    std::future<void> stopFuture;
    bool scanInFlight = false;
    bool startInFlight = false;
    bool rescanInFlight = false;
    bool stopInFlight = false;
    bool bridgeSyncPending = false;
    bool showLogsPopup = false;
    bool showPluginsWindow = false;
    std::set<std::string> enabledControllers; // IDs of controllers selected for bridging

    auto launchSelectedStart = [&](std::set<std::string> selectedIds) {
        if (selectedIds.empty()) {
            return;
        }

        const idn_bridge::BridgeOptions options;
        startFuture = std::async(std::launch::async,
                                 [&runtime, options, selectedIds = std::move(selectedIds)] {
                                     return runtime.start(options, selectedIds);
                                 });
        startInFlight = true;
    };

    auto launchStop = [&]() {
        stopFuture = std::async(std::launch::async, [&runtime] {
            runtime.stop();
        });
        stopInFlight = true;
    };

    while (app.beginFrame()) {
        if (scanFuture.valid() && scanFuture.wait_for(0ms) == std::future_status::ready) {
            (void)scanFuture.get();
            scanInFlight = false;
            bridgeSyncPending = true;
        }
        if (startFuture.valid() && startFuture.wait_for(0ms) == std::future_status::ready) {
            (void)startFuture.get();
            startInFlight = false;
            rescanInFlight = false;
        }
        if (stopFuture.valid() && stopFuture.wait_for(0ms) == std::future_status::ready) {
            stopFuture.get();
            stopInFlight = false;
        }

        const auto snapshot = runtime.snapshot();
        if (snapshot.hasDiscoveryResults) {
            std::set<std::string> bridgeableControllerIds;
            for (const auto& controller : snapshot.discovered) {
                if (controller.bridgeable) {
                    bridgeableControllerIds.insert(controller.id);
                }
            }

            for (auto it = enabledControllers.begin(); it != enabledControllers.end();) {
                if (bridgeableControllerIds.find(*it) == bridgeableControllerIds.end()) {
                    it = enabledControllers.erase(it);
                } else {
                    ++it;
                }
            }
        }

        std::set<std::string> activeControllerIds;
        for (const auto& endpoint : snapshot.endpoints) {
            activeControllerIds.insert(endpoint.id);
        }

        auto selectedBridgeableIds = [&]() {
            std::set<std::string> selectedIds;
            for (const auto& controller : snapshot.discovered) {
                if (controller.bridgeable && enabledControllers.count(controller.id) > 0) {
                    selectedIds.insert(controller.id);
                }
            }
            return selectedIds;
        };

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Lib Port", nullptr,
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar);

        const float logoHeight = drawBrandLogo(app,
                                               ImGui::GetCursorScreenPos(),
                                               ImGui::GetContentRegionAvail().x,
                                               false);
        if (logoHeight > 0.0f) {
            ImGui::Dummy(ImVec2(0.0f, logoHeight + 12.0f));
        }

        ImGui::BeginGroup();
        ImGui::Text("Bridge Status");
        ImGui::SameLine();
        ImGui::TextColored(statusColor(snapshot.state), "%s", idn_bridge::runtimeStateLabel(snapshot.state));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", snapshot.statusMessage.c_str());

        ImGui::Spacing();

        if (scanInFlight) {
            ImGui::BeginDisabled();
            ImGui::Button("Scanning...", ImVec2(140.0f, 0.0f));
            ImGui::EndDisabled();
        } else if (rescanInFlight) {
            ImGui::BeginDisabled();
            ImGui::Button("Rescanning...", ImVec2(140.0f, 0.0f));
            ImGui::EndDisabled();
        } else {
            const bool bridgeRunning = snapshot.state == idn_bridge::RuntimeState::Running ||
                                       snapshot.state == idn_bridge::RuntimeState::StopRequested;
            const char* scanButtonLabel = bridgeRunning ? "Rescan" : "Scan";
            const bool scanActionDisabled = startInFlight || stopInFlight;
            if (scanActionDisabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(scanButtonLabel, ImVec2(140.0f, 0.0f))) {
                if (bridgeRunning) {
                    rescanInFlight = true;
                    bridgeSyncPending = false;
                } else {
                    const idn_bridge::BridgeOptions options;
                    scanFuture = std::async(std::launch::async, [&runtime, options] {
                        return runtime.scan(options);
                    });
                    scanInFlight = true;
                }
            }
            if (scanActionDisabled) {
                ImGui::EndDisabled();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FK_PLUS_CIRCLE "  Plugins", ImVec2(140.0f, 0.0f))) {
            showPluginsWindow = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Logs", ImVec2(100.0f, 0.0f))) {
            showLogsPopup = true;
            ImGui::OpenPopup("Logs");
        }

        ImGui::SameLine();
        ImGui::TextDisabled("Controllers: %zu discovered / %zu active",
                            snapshot.discoveredControllers,
                            snapshot.startedEndpoints);
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Detected DACs");
        if (ImGui::BeginTable("DiscoveredControllers",
                              7,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
            const float enableSz = ImGui::GetFrameHeight() * 0.8f;

            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            ImGui::TableSetupColumn("Bridge", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            ImGui::TableSetupColumn("Label");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Max PPS");
            ImGui::TableSetupColumn("Usage");
            ImGui::TableSetupColumn("Note");
            ImGui::TableHeadersRow();

            for (const auto& controller : snapshot.discovered) {
                ImGui::PushID(controller.id.c_str());

                bool selected = enabledControllers.count(controller.id) > 0;
                const bool active = activeControllerIds.count(controller.id) > 0;
                const ImU32 statusCol = active
                                            ? IM_COL32(0, 255, 0, 255)
                                            : !controller.bridgeable
                                                  ? IM_COL32(242, 199, 92, 255)
                                                  : selected
                                                        ? IM_COL32(66, 150, 250, 255)
                                                        : IM_COL32(77, 77, 77, 255);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                libera::widgets::statusSquare("status", statusCol, enableSz);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s", controller.label.c_str());
                    ImGui::Text("Type: %s", controller.type.c_str());
                    if (controller.maxPointRate > 0) {
                        ImGui::Text("Max: %u pps", controller.maxPointRate);
                    }
                    if (active) {
                        ImGui::TextUnformatted("Bridge active");
                    } else if (!controller.bridgeable) {
                        ImGui::TextUnformatted(controller.note.c_str());
                    } else if (selected) {
                        ImGui::TextUnformatted("Enabled for bridging");
                    } else {
                        ImGui::TextUnformatted("Disabled");
                    }
                    ImGui::EndTooltip();
                }

                ImGui::TableNextColumn();
                if (!controller.bridgeable) {
                    ImGui::BeginDisabled();
                }
                const bool toggled = libera::widgets::toggleButton("enable", &selected, false, enableSz);
                if (!controller.bridgeable) {
                    ImGui::EndDisabled();
                }
                if (selected) {
                    enabledControllers.insert(controller.id);
                } else {
                    enabledControllers.erase(controller.id);
                }
                if (toggled) {
                    bridgeSyncPending = true;
                    if (startInFlight) {
                        runtime.requestStop();
                    }
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(controller.label.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(controller.type.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", controller.maxPointRate);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(controller.usage.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(controller.note.c_str());

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        if (snapshot.discovered.empty()) {
            if (snapshot.hasDiscoveryResults) {
                ImGui::TextDisabled("No DACs found.");
            } else {
                ImGui::TextDisabled("Click Scan to look for DACs before enabling any bridge connections.");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Active Endpoints");
        if (ImGui::BeginTable("Endpoints", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Service");
            ImGui::TableSetupColumn("Label");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Output PPS");
            ImGui::TableSetupColumn("Input PPS");
            ImGui::TableSetupColumn("Latency");
            ImGui::TableSetupColumn("Queue");
            ImGui::TableSetupColumn("Underruns");
            ImGui::TableSetupColumn("Dropped");
            ImGui::TableSetupColumn("State");
            ImGui::TableHeadersRow();

            for (const auto& endpoint : snapshot.endpoints) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", static_cast<unsigned>(endpoint.serviceId));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(endpoint.label.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(endpoint.type.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%u", endpoint.stats.outputPointRate);
                ImGui::TableNextColumn(); ImGui::Text("%u", endpoint.stats.observedInputPointRate);
                ImGui::TableNextColumn(); ImGui::Text("%ums", endpoint.stats.latencyMs);
                ImGui::TableNextColumn(); ImGui::Text("%zu / %zu",
                                                      endpoint.stats.queuedPoints,
                                                      endpoint.stats.targetBufferedPoints);
                ImGui::TableNextColumn(); ImGui::Text("%llu",
                                                      static_cast<unsigned long long>(endpoint.stats.callbackUnderrunEvents));
                ImGui::TableNextColumn(); ImGui::Text("%llu",
                                                      static_cast<unsigned long long>(endpoint.stats.droppedPoints));
                ImGui::TableNextColumn();
                ImGui::TextColored(endpoint.stats.buffering
                                       ? ImVec4(0.95f, 0.78f, 0.32f, 1.0f)
                                       : ImVec4(0.40f, 0.88f, 0.55f, 1.0f),
                                   "%s",
                                   endpoint.stats.buffering ? "Buffering" : "Streaming");
            }

            ImGui::EndTable();
        }

        if (snapshot.endpoints.empty()) {
            ImGui::TextDisabled("No active endpoints.");
        }

        ImGui::End();

        ImGui::SetNextWindowSize(ImVec2(900.0f, 420.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Logs", &showLogsPopup, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::BeginChild("LogsPopupContent", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), true);
            if (snapshot.recentLogs.empty()) {
                ImGui::TextDisabled("No logs yet.");
            } else {
                for (const auto& line : snapshot.recentLogs) {
                    if (!snapshot.lastError.empty() && line == snapshot.lastError) {
                        ImGui::TextColored(ImVec4(0.95f, 0.34f, 0.34f, 1.0f), "%s", line.c_str());
                    } else {
                        ImGui::TextUnformatted(line.c_str());
                    }
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();

            if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
                showLogsPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        const auto targetControllerIds = selectedBridgeableIds();
        const bool bridgeRunning = snapshot.state == idn_bridge::RuntimeState::Running ||
                                   snapshot.state == idn_bridge::RuntimeState::StopRequested;
        const bool bridgeSelectionMatches = activeControllerIds == targetControllerIds;

        if (!scanInFlight && !startInFlight && !stopInFlight) {
            if (rescanInFlight) {
                if (bridgeRunning) {
                    launchStop();
                } else if (!targetControllerIds.empty()) {
                    bridgeSyncPending = false;
                    launchSelectedStart(targetControllerIds);
                } else {
                    rescanInFlight = false;
                }
            } else if (bridgeSyncPending) {
                if (bridgeRunning) {
                    if (!bridgeSelectionMatches) {
                        launchStop();
                    } else {
                        bridgeSyncPending = false;
                    }
                } else {
                    if (!targetControllerIds.empty()) {
                        bridgeSyncPending = false;
                        launchSelectedStart(targetControllerIds);
                    } else {
                        bridgeSyncPending = false;
                    }
                }
            }
        }

        libera::ui::DrawPluginsWindow(&showPluginsWindow, idn_bridge::userPluginDirectory());
        app.endFrame();
    }

    runtime.requestStop();
    if (scanFuture.valid()) {
        scanFuture.wait();
    }
    if (startFuture.valid()) {
        startFuture.wait();
    }
    if (stopFuture.valid()) {
        stopFuture.wait();
    }
    runtime.stop();
    app.shutdown();
    return 0;
}
