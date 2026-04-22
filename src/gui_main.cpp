#include "BridgeRuntime.hpp"
#include "LiberaApp.h"
#include "LiberaPaths.hpp"
#include "LiberaPluginsWindow.h"
#include "LiberaWidgets.h"

#include "fonts/IconsForkAwesome.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <future>
#include <set>
#include <unordered_map>
#include <utility>
#include <string>

namespace {

using namespace std::chrono_literals;

ImVec4 statusColor(libera_link::RuntimeState state) {
    switch (state) {
    case libera_link::RuntimeState::Running:
        return ImVec4(0.40f, 0.88f, 0.55f, 1.0f);
    case libera_link::RuntimeState::Scanning:
    case libera_link::RuntimeState::Starting:
    case libera_link::RuntimeState::StopRequested:
        return ImVec4(0.95f, 0.78f, 0.32f, 1.0f);
    case libera_link::RuntimeState::Failed:
        return ImVec4(0.95f, 0.34f, 0.34f, 1.0f);
    case libera_link::RuntimeState::Stopped:
    default:
        return ImVec4(0.68f, 0.72f, 0.78f, 1.0f);
    }
}

void drawSectionTitle(const LiberaApp& app, const char* title, const char* subtitle = nullptr) {
    if (app.fontMedium) {
        ImGui::PushFont(app.fontMedium);
    }
    ImGui::TextUnformatted(title);
    if (app.fontMedium) {
        ImGui::PopFont();
    }
    if (subtitle && *subtitle) {
        ImGui::TextDisabled("%s", subtitle);
    }
}

float drawBrandLogo(const LiberaApp& app, ImVec2 pos, float areaWidth, bool rightAlign = false) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImFont* boldFont = app.fontLarge ? app.fontLarge : ImGui::GetFont();
    ImFont* defaultFont = app.fontBase ? app.fontBase : ImGui::GetFont();
    const float fontScale = ImGui::GetStyle().FontScaleMain;

    const float logoFontSize = boldFont->LegacySize * fontScale;
    const float baseSubFontSize = defaultFont->LegacySize * fontScale * 0.85f;
    const char* firstWord = "LIBERA";
    const char* secondWord = "LINK";
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

void drawQueueBreakdown(std::size_t localQueuedPoints,
                        std::size_t prefetchedPoints,
                        std::size_t transportBufferedPoints) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float availableWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    const float lineHeight = ImGui::GetTextLineHeight();
    const float separatorGap = 6.0f;
    const char* separator = "+";
    const float separatorWidth = ImGui::CalcTextSize(separator).x;
    const float separatorBlockWidth = separatorWidth + (separatorGap * 2.0f);
    const float slotWidth = std::max(
        1.0f,
        (availableWidth - (separatorBlockWidth * 2.0f)) / 3.0f);
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);

    auto drawValueInSlot = [&](float slotStartX, std::size_t value) {
        const std::string text = std::to_string(value);
        const float textWidth = ImGui::CalcTextSize(text.c_str()).x;
        const float x = slotStartX + std::max(0.0f, slotWidth - textWidth);
        drawList->AddText(ImVec2(x, origin.y), textColor, text.c_str());
    };

    const float firstSlotX = origin.x;
    const float firstSeparatorX = firstSlotX + slotWidth + separatorGap;
    const float secondSlotX = firstSlotX + slotWidth + separatorBlockWidth;
    const float secondSeparatorX = secondSlotX + slotWidth + separatorGap;
    const float thirdSlotX = secondSlotX + slotWidth + separatorBlockWidth;

    drawValueInSlot(firstSlotX, localQueuedPoints);
    drawList->AddText(ImVec2(firstSeparatorX, origin.y), textColor, separator);
    drawValueInSlot(secondSlotX, prefetchedPoints);
    drawList->AddText(ImVec2(secondSeparatorX, origin.y), textColor, separator);
    drawValueInSlot(thirdSlotX, transportBufferedPoints);

    ImGui::Dummy(ImVec2(availableWidth, lineHeight));
}
} // namespace

int main() {
    LiberaApp app;
    if (!app.init({"Lib Port", 1180, 780, -1, -1})) {
        return 1;
    }

    libera_link::BridgeRuntime runtime;

    std::future<bool> scanFuture;
    std::future<bool> startFuture;
    std::future<void> stopFuture;
    bool scanInFlight = false;
    bool startInFlight = false;
    bool rescanInFlight = false;
    bool stopInFlight = false;
    bool bridgeSyncPending = false;
    bool showLogsWindow = false;
    bool showPluginsWindow = false;
    std::set<std::string> enabledControllers; // IDs of controllers selected for bridging

    auto launchSelectedStart = [&](std::set<std::string> selectedIds) {
        if (selectedIds.empty()) {
            return;
        }

        const libera_link::BridgeOptions options;
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

    {
        const libera_link::BridgeOptions options;
        scanFuture = std::async(std::launch::async, [&runtime, options] {
            return runtime.scan(options);
        });
        scanInFlight = true;
    }

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
        std::unordered_map<std::string, const libera_link::EndpointSnapshot*> endpointByControllerId;
        endpointByControllerId.reserve(snapshot.endpoints.size());
        for (const auto& endpoint : snapshot.endpoints) {
            activeControllerIds.insert(endpoint.id);
            endpointByControllerId.emplace(endpoint.id, &endpoint);
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

        auto selectedBridgeableCount = [&]() -> std::size_t {
            std::size_t count = 0;
            for (const auto& controller : snapshot.discovered) {
                if (controller.bridgeable && enabledControllers.count(controller.id) > 0) {
                    ++count;
                }
            }
            return count;
        };

        auto bridgeableCount = [&]() -> std::size_t {
            return static_cast<std::size_t>(std::count_if(
                snapshot.discovered.begin(), snapshot.discovered.end(),
                [](const libera_link::DiscoveredControllerSnapshot& controller) {
                    return controller.bridgeable;
                }));
        };

        const auto runtimeLabel = libera_link::runtimeStateLabel(snapshot.state);
        const ImVec4 runtimeColor = statusColor(snapshot.state);

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Lib Port", nullptr,
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        const float logoHeight = drawBrandLogo(app,
                                               ImGui::GetCursorScreenPos(),
                                               ImGui::GetContentRegionAvail().x,
                                               false);
        if (logoHeight > 0.0f) {
            ImGui::Dummy(ImVec2(0.0f, logoHeight + 12.0f));
        }

        const bool showTopError = !snapshot.lastError.empty() &&
                                  snapshot.state == libera_link::RuntimeState::Failed;
        const float overviewHeight =
            (ImGui::GetFrameHeightWithSpacing() * 1.35f) +
            (ImGui::GetTextLineHeightWithSpacing() * (showTopError ? 3.1f : 2.35f)) +
            12.0f;
        ImGui::BeginChild("BridgeOverview", ImVec2(0.0f, overviewHeight), true, ImGuiWindowFlags_NoScrollbar);

        if (scanInFlight) {
            ImGui::BeginDisabled();
            ImGui::Button("Scanning...", ImVec2(140.0f, 0.0f));
            ImGui::EndDisabled();
        } else if (rescanInFlight) {
            ImGui::BeginDisabled();
            ImGui::Button("Rescanning...", ImVec2(140.0f, 0.0f));
            ImGui::EndDisabled();
        } else {
            const bool bridgeRunning = snapshot.state == libera_link::RuntimeState::Running ||
                                       snapshot.state == libera_link::RuntimeState::StopRequested;
            const char* scanButtonLabel = "RESCAN";
            const bool scanActionDisabled = startInFlight || stopInFlight;
            if (scanActionDisabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(scanButtonLabel, ImVec2(140.0f, 0.0f))) {
                if (bridgeRunning) {
                    rescanInFlight = true;
                    bridgeSyncPending = false;
                } else {
                    const libera_link::BridgeOptions options;
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
        if (ImGui::Button("Logs", ImVec2(100.0f, 0.0f))) {
            showLogsWindow = true;
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FK_PLUS_CIRCLE "  Plugins", ImVec2(140.0f, 0.0f))) {
            showPluginsWindow = true;
        }

        ImGui::Spacing();
        ImGui::TextColored(runtimeColor, "%s", runtimeLabel);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", snapshot.statusMessage.c_str());

        if (!snapshot.lastError.empty() && snapshot.state == libera_link::RuntimeState::Failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.34f, 0.34f, 1.0f), "%s", snapshot.lastError.c_str());
        } else {
            ImGui::TextDisabled("%zu bridgeable controllers | %zu enabled | %zu active endpoints",
                                bridgeableCount(),
                                selectedBridgeableCount(),
                                snapshot.startedEndpoints);
        }
        ImGui::EndChild();

        ImGui::Spacing();

        ImGui::BeginChild("DetectedDacsPanel", ImVec2(0.0f, 0.0f), true);
        drawSectionTitle(app, "Detected Controllers");

        if (!snapshot.discovered.empty() &&
            ImGui::BeginTable("DiscoveredControllers",
                              9,
                              ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_ScrollX,
                              ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
            const float enableSz = ImGui::GetFrameHeight() * 0.8f;
            const float controlGap = 7.0f;
            const float rowHeight = ImGui::GetFrameHeight();
            
            ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Max PPS", ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("Rates", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthFixed, 220.0f);
            ImGui::TableSetupColumn("Health", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (const auto& controller : snapshot.discovered) {
                ImGui::PushID(controller.id.c_str());

                const auto endpointIt = endpointByControllerId.find(controller.id);
                const libera_link::EndpointSnapshot* endpoint =
                    endpointIt != endpointByControllerId.end() ? endpointIt->second : nullptr;
                bool selected = enabledControllers.count(controller.id) > 0;
                const bool active = endpoint != nullptr;
                const bool disabled = !controller.bridgeable;
                const bool pendingSelectionChange =
                    controller.bridgeable && selected && !active &&
                    (startInFlight || stopInFlight || bridgeSyncPending || rescanInFlight);
                const bool buffering = endpoint && endpoint->stats.buffering;
                const bool hasUnderruns =
                    endpoint && endpoint->stats.callbackUnderrunEvents > 0;
                const bool hasDrops =
                    endpoint && endpoint->stats.droppedPoints > 0;
                const bool unhealthy = endpoint && (buffering || hasUnderruns || hasDrops);

                const ImU32 statusCol = active
                                            ? IM_COL32(0, 255, 0, 255)
                                            : !controller.bridgeable
                                                  ? IM_COL32(242, 199, 92, 255)
                                                  : selected
                                                        ? IM_COL32(66, 150, 250, 255)
                                                        : IM_COL32(77, 77, 77, 255);
                std::string stateLabel;
                ImVec4 stateTextColor = ImVec4(0.68f, 0.72f, 0.78f, 1.0f);

                if (!controller.bridgeable) {
                    stateLabel = "Unavailable";
                    stateTextColor = ImVec4(0.95f, 0.78f, 0.32f, 1.0f);
                } else if (active) {
                    stateLabel = "Active";
                    stateTextColor = ImVec4(0.40f, 0.88f, 0.55f, 1.0f);
                } else if (pendingSelectionChange) {
                    stateLabel = "Pending";
                    stateTextColor = ImVec4(0.95f, 0.78f, 0.32f, 1.0f);
                } else if (selected) {
                    stateLabel = "Enabled";
                    stateTextColor = ImVec4(0.53f, 0.76f, 1.0f, 1.0f);
                } else {
                    stateLabel = "Idle";
                    stateTextColor = ImVec4(0.68f, 0.72f, 0.78f, 1.0f);
                }

                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                auto drawRowTooltip = [&]() {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s", controller.label.c_str());
                    ImGui::TextDisabled("ID: %s", controller.id.c_str());
                    ImGui::Text("Type: %s", controller.type.c_str());
                    ImGui::Text("Usage: %s", controller.usage.c_str());
                    if (controller.maxPointRate > 0) {
                        ImGui::Text("Max: %u pps", controller.maxPointRate);
                    }
                    if (!controller.note.empty()) {
                        ImGui::TextWrapped("%s", controller.note.c_str());
                    }
                    if (endpoint) {
                        ImGui::Separator();
                        ImGui::Text("Service: %u", static_cast<unsigned>(endpoint->serviceId));
                        ImGui::Text("Input: %u pps", endpoint->stats.observedInputPointRate);
                        ImGui::Text("Output: %u pps", endpoint->stats.outputPointRate);
                        ImGui::Text("Latency: %ums", endpoint->stats.latencyMs);
                        ImGui::Text("Buffered: %zu / %zu",
                                    endpoint->stats.totalBufferedPoints,
                                    endpoint->stats.targetBufferedPoints);
                        ImGui::Text("Local queue: %zu", endpoint->stats.queuedPoints);
                        ImGui::Text("Controller: %zu", endpoint->stats.controllerBufferedPoints);
                        ImGui::Text("Controller prefetch: %zu",
                                    endpoint->stats.controllerPrefetchedPoints);
                        ImGui::Text("Controller transport: %zu",
                                    endpoint->stats.controllerTransportBufferedPoints);
                        ImGui::Text("Blank fill: %llu",
                                    static_cast<unsigned long long>(endpoint->stats.blankFillPoints));
                        ImGui::Text("Underruns: %llu",
                                    static_cast<unsigned long long>(endpoint->stats.callbackUnderrunEvents));
                        ImGui::Text("Dropped: %llu",
                                    static_cast<unsigned long long>(endpoint->stats.droppedPoints));
                    }
                    ImGui::EndTooltip();
                };

                ImGui::TableNextColumn();
                const bool toggled =
                    libera::widgets::toggleButton("enable", &selected, pendingSelectionChange, enableSz, nullptr, disabled);
                ImGui::SameLine(0.0f, controlGap);
                libera::widgets::statusSquare("status", statusCol, enableSz);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    drawRowTooltip();
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
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(controller.label.c_str());
                if (ImGui::IsItemHovered()) {
                    drawRowTooltip();
                }

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(stateTextColor, "%s", stateLabel.c_str());

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (controller.maxPointRate > 0) {
                    ImGui::Text("%u", controller.maxPointRate);
                } else {
                    ImGui::TextDisabled("n/a");
                }

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (endpoint) {
                    ImGui::Text("%u", static_cast<unsigned>(endpoint->serviceId));
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (endpoint) {
                    ImGui::Text("O %u / I %u",
                                endpoint->stats.outputPointRate,
                                endpoint->stats.observedInputPointRate);
                } else {
                    ImGui::TextDisabled("Not active");
                }

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (endpoint) {
                    ImGui::Text("%ums", endpoint->stats.latencyMs);
                } else {
                    ImGui::TextDisabled("-");
                }
                
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (endpoint) {
                    drawQueueBreakdown(endpoint->stats.queuedPoints,
                                       endpoint->stats.controllerPrefetchedPoints,
                                       endpoint->stats.controllerTransportBufferedPoints);
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (endpoint) {
                    if (buffering) {
                        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.32f, 1.0f), "%s", "Buffering");
                    } else if (unhealthy) {
                        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.32f, 1.0f), "%s", "Issues");
                    } else {
                        ImGui::TextColored(ImVec4(0.40f, 0.88f, 0.55f, 1.0f), "%s", "Healthy");
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        if (buffering) {
                            ImGui::TextWrapped("The controller is still filling its target buffer before steady playback.");
                        }
                        if (hasUnderruns) {
                            ImGui::Text("Underruns: %llu",
                                        static_cast<unsigned long long>(endpoint->stats.callbackUnderrunEvents));
                            ImGui::TextWrapped("The bridge had to generate blank fallback points because not enough queued points were ready.");
                        }
                        if (hasDrops) {
                            ImGui::Text("Dropped points: %llu",
                                        static_cast<unsigned long long>(endpoint->stats.droppedPoints));
                            ImGui::TextWrapped("Old queued points were discarded because the bridge queue hit its maximum size.");
                        }
                        if (!buffering && !hasUnderruns && !hasDrops) {
                            ImGui::TextWrapped("No buffering, underruns, or dropped points have been recorded for this controller since it was started.");
                        } else {
                            ImGui::Separator();
                            ImGui::TextDisabled("These counters are cumulative for the current bridge run.");
                        }
                        ImGui::EndTooltip();
                    }
                } else {
                    ImGui::TextDisabled("Not active");
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        if (snapshot.discovered.empty()) {
            if (snapshot.hasDiscoveryResults) {
                ImGui::TextWrapped("No controllers were found during the last scan. Check connections or plugins, then rescan.");
            } else {
                ImGui::TextWrapped("No scan results yet. Click RESCAN to discover controllers, then enable the ones you want to bridge.");
            }
        } else if (bridgeableCount() == 0) {
            ImGui::TextWrapped("Controllers were discovered, but none of them are currently bridgeable.");
        }
        ImGui::EndChild();

        ImGui::End();

        if (showLogsWindow) {
            ImGui::SetNextWindowSize(ImVec2(900.0f, 420.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Bridge Logs", &showLogsWindow, ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextDisabled("Recent runtime messages. This window stays open while you keep working.");
                ImGui::Separator();
                ImGui::BeginChild("LogsWindowContent", ImVec2(0.0f, 0.0f), true);
                const bool stickToBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
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
                    if (stickToBottom) {
                        ImGui::SetScrollHereY(1.0f);
                    }
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        const auto targetControllerIds = selectedBridgeableIds();
        const bool bridgeRunning = snapshot.state == libera_link::RuntimeState::Running ||
                                   snapshot.state == libera_link::RuntimeState::StopRequested;
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

        libera::ui::DrawPluginsWindow(&showPluginsWindow, libera_link::userPluginDirectory());
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
