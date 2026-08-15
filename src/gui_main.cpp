#include "LinkRuntime.hpp"
#include "LiberaApp.h"
#include "LiberaPaths.hpp"
#include "LiberaPluginsWindow.h"
#include "libera/System.hpp"
#include "virtual_controller/VirtualControllerHostRegistry.hpp"

#include "fonts/IconsForkAwesome.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <unordered_map>
#include <utility>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::filesystem::path disabledControllerTypesPath() {
    return std::filesystem::path(libera_link::settingsDirectory()) /
           "disabled-controller-types.txt";
}

std::filesystem::path defaultVirtualControllerHostPath() {
    return std::filesystem::path(libera_link::settingsDirectory()) /
           "default-virtual-controller-host.txt";
}

std::filesystem::path controllerHostRoutesPath() {
    return std::filesystem::path(libera_link::settingsDirectory()) /
           "controller-host-routes.txt";
}

std::filesystem::path enabledControllersPath() {
    return std::filesystem::path(libera_link::settingsDirectory()) /
           "enabled-controllers.txt";
}

std::string trimSettingLine(const std::string& line) {
    const auto begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = line.find_last_not_of(" \t\r\n");
    return line.substr(begin, end - begin + 1);
}

std::set<std::string>
loadDisabledControllerTypes(const std::set<std::string>& fallbackDisabledTypes) {
    if (!std::filesystem::exists(disabledControllerTypesPath())) {
        return fallbackDisabledTypes;
    }

    std::set<std::string> disabledTypes;
    std::ifstream input(disabledControllerTypesPath());
    std::string line;
    while (std::getline(input, line)) {
        line = trimSettingLine(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        disabledTypes.insert(std::move(line));
    }
    return disabledTypes;
}

void saveDisabledControllerTypes(const std::set<std::string>& disabledTypes) {
    std::ofstream output(disabledControllerTypesPath(), std::ios::trunc);
    for (const auto& type : disabledTypes) {
        output << type << '\n';
    }
}

std::string loadDefaultVirtualControllerHostId() {
    std::ifstream input(defaultVirtualControllerHostPath());
    std::string line;
    if (std::getline(input, line)) {
        return trimSettingLine(line);
    }
    return {};
}

void saveDefaultVirtualControllerHostId(const std::string& hostId) {
    std::ofstream output(defaultVirtualControllerHostPath(), std::ios::trunc);
    if (!hostId.empty()) {
        output << hostId << '\n';
    }
}

std::unordered_map<std::string, std::string> loadControllerHostRoutes() {
    std::unordered_map<std::string, std::string> routes;
    std::ifstream input(controllerHostRoutesPath());
    std::string line;
    while (std::getline(input, line)) {
        line = trimSettingLine(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }

        std::string controllerId = trimSettingLine(line.substr(0, tab));
        std::string hostId = trimSettingLine(line.substr(tab + 1));
        if (!controllerId.empty() && !hostId.empty()) {
            routes[std::move(controllerId)] = std::move(hostId);
        }
    }
    return routes;
}

void saveControllerHostRoutes(const std::unordered_map<std::string, std::string>& routes) {
    std::ofstream output(controllerHostRoutesPath(), std::ios::trunc);
    std::vector<std::pair<std::string, std::string>> sortedRoutes(
        routes.begin(), routes.end());
    std::sort(sortedRoutes.begin(), sortedRoutes.end());
    for (const auto& route : sortedRoutes) {
        if (!route.first.empty() && !route.second.empty()) {
            output << route.first << '\t' << route.second << '\n';
        }
    }
}

std::set<std::string> loadEnabledControllers() {
    std::set<std::string> enabledControllers;
    std::ifstream input(enabledControllersPath());
    std::string line;
    while (std::getline(input, line)) {
        line = trimSettingLine(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        enabledControllers.insert(std::move(line));
    }
    return enabledControllers;
}

void saveEnabledControllers(const std::set<std::string>& enabledControllers) {
    std::ofstream output(enabledControllersPath(), std::ios::trunc);
    for (const auto& controllerId : enabledControllers) {
        output << controllerId << '\n';
    }
}

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

void drawStatusLight(ImDrawList* drawList,
                     ImVec2 center,
                     float radius,
                     ImU32 color,
                     bool glow) {
    if (glow) {
        drawList->AddCircleFilled(center, radius * 2.1f, color & IM_COL32(255, 255, 255, 55), 24);
    }
    drawList->AddCircleFilled(center, radius, color, 24);
    drawList->AddCircle(center, radius, IM_COL32(255, 255, 255, 110), 24, 1.0f);
}

void drawClippedText(ImDrawList* drawList,
                     ImVec2 min,
                     ImVec2 max,
                     const std::string& text,
                     ImU32 color) {
    if (text.empty() || max.x <= min.x || max.y <= min.y) {
        return;
    }

    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const float y = min.y + std::max(0.0f, (max.y - min.y - textSize.y) * 0.5f);
    drawList->PushClipRect(min, max, true);
    drawList->AddText(ImVec2(min.x, y), color, text.c_str());
    drawList->PopClipRect();
}

void drawLinkNode(ImDrawList* drawList,
                  ImVec2 min,
                  ImVec2 max,
                  const std::string& label,
                  ImU32 fillColor,
                  ImU32 borderColor,
                  ImU32 textColor,
                  ImU32 lightColor,
                  bool lightGlow,
                  const std::string& secondaryLabel = std::string(),
                  ImU32 secondaryTextColor = IM_COL32(168, 178, 190, 235)) {
    const float rounding = 8.0f;
    drawList->AddRectFilled(min, max, fillColor, rounding);
    drawList->AddRect(min, max, borderColor, rounding, 0, 1.2f);

    const float lightRadius = 5.5f;
    const ImVec2 lightCenter(min.x + 18.0f, (min.y + max.y) * 0.5f);
    drawStatusLight(drawList, lightCenter, lightRadius, lightColor, lightGlow);

    const ImVec2 textMin(min.x + 34.0f, min.y + 4.0f);
    const ImVec2 textMax(max.x - 12.0f, max.y - 4.0f);
    if (secondaryLabel.empty()) {
        drawClippedText(drawList, textMin, textMax, label, textColor);
        return;
    }

    const float splitY = min.y + (max.y - min.y) * 0.52f;
    drawClippedText(drawList,
                    ImVec2(textMin.x, min.y + 2.0f),
                    ImVec2(textMax.x, splitY + 1.0f),
                    label,
                    textColor);
    drawClippedText(drawList,
                    ImVec2(textMin.x, splitY - 1.0f),
                    ImVec2(textMax.x, max.y - 2.0f),
                    secondaryLabel,
                    secondaryTextColor);
}

void subtractWireGap(std::vector<std::pair<float, float>>& segments, float gapStart, float gapEnd) {
    if (gapEnd <= gapStart) {
        return;
    }

    std::vector<std::pair<float, float>> next;
    for (const auto& segment : segments) {
        if (gapEnd <= segment.first || gapStart >= segment.second) {
            next.push_back(segment);
            continue;
        }
        if (gapStart > segment.first) {
            next.emplace_back(segment.first, gapStart);
        }
        if (gapEnd < segment.second) {
            next.emplace_back(gapEnd, segment.second);
        }
    }
    segments = std::move(next);
}

void drawVirtualWire(ImDrawList* drawList,
                     ImVec2 start,
                     ImVec2 end,
                     bool lit,
                     bool flowing,
                     bool hostBreak,
                     bool controllerBreak) {
    if (end.x <= start.x + 4.0f) {
        return;
    }

    const float wireLength = end.x - start.x;
    const float y = start.y;
    const float gapWidth = std::min(24.0f, std::max(10.0f, wireLength * 0.12f));
    std::vector<std::pair<float, float>> segments{{start.x, end.x}};
    std::vector<std::pair<float, float>> gaps;

    if (hostBreak) {
        const float gapCenter = start.x + std::min(44.0f, wireLength * 0.32f);
        const float gapStart = gapCenter - gapWidth * 0.5f;
        const float gapEnd = gapCenter + gapWidth * 0.5f;
        subtractWireGap(segments, gapStart, gapEnd);
        gaps.emplace_back(gapStart, gapEnd);
    }

    if (controllerBreak) {
        const float gapCenter = end.x - std::min(44.0f, wireLength * 0.32f);
        const float gapStart = gapCenter - gapWidth * 0.5f;
        const float gapEnd = gapCenter + gapWidth * 0.5f;
        subtractWireGap(segments, gapStart, gapEnd);
        gaps.emplace_back(gapStart, gapEnd);
    }

    const ImU32 baseColor = lit ? IM_COL32(20, 88, 56, 255) : IM_COL32(87, 98, 112, 210);
    const ImU32 flowColor = IM_COL32(66, 213, 142, 255);
    const ImU32 shadowColor = IM_COL32(17, 22, 28, 200);
    for (const auto& segment : segments) {
        if (segment.second <= segment.first) {
            continue;
        }
        drawList->AddLine(ImVec2(segment.first, y + 1.0f),
                          ImVec2(segment.second, y + 1.0f),
                          shadowColor,
                          6.0f);
        drawList->AddLine(ImVec2(segment.first, y),
                          ImVec2(segment.second, y),
                          baseColor,
                          lit ? 3.2f : 2.2f);
    }

    if (flowing) {
        const float spacing = 26.0f;
        const float stripeLength = 14.0f;
        const float offset = static_cast<float>(std::fmod(ImGui::GetTime() * 150.0, spacing));
        const float slant = 4.0f;
        for (float x = start.x - spacing + offset; x < end.x; x += spacing) {
            for (const auto& segment : segments) {
                const float stripeStart = std::max(x, segment.first);
                const float stripeEnd = std::min(x + stripeLength, segment.second);
                if (stripeEnd > stripeStart + 2.0f) {
                    drawList->AddQuadFilled(ImVec2(stripeStart + slant, y - 2.1f),
                                            ImVec2(stripeEnd + slant, y - 2.1f),
                                            ImVec2(stripeEnd - slant, y + 2.1f),
                                            ImVec2(stripeStart - slant, y + 2.1f),
                                            flowColor);
                }
            }
        }
    }

    const ImU32 breakColor = IM_COL32(238, 87, 78, 255);
    for (const auto& gap : gaps) {
        const float leftX = gap.first + 3.0f;
        const float rightX = gap.second - 3.0f;
        drawList->AddLine(ImVec2(leftX, y - 8.0f), ImVec2(leftX + 8.0f, y + 8.0f), breakColor, 2.2f);
        drawList->AddLine(ImVec2(rightX - 8.0f, y - 8.0f), ImVec2(rightX, y + 8.0f), breakColor, 2.2f);
    }
}
} // namespace

int main() {
    LiberaApp app;
    if (!app.init({"Libera Link", 505, 900, -1, -1})) {
        return 1;
    }

    libera_link::LinkRuntime runtime;
    libera_link::LinkOptions linkOptions;
    if (const auto defaultVirtualControllerHost = libera_link::virtual_controller::defaultVirtualControllerHost()) {
        linkOptions.virtualControllerHostId = defaultVirtualControllerHost->id;
    }
    if (const auto savedVirtualControllerHostId = loadDefaultVirtualControllerHostId();
        !savedVirtualControllerHostId.empty()) {
        linkOptions.virtualControllerHostId = savedVirtualControllerHostId;
    }
    std::set<std::string> disabledControllerTypes =
        loadDisabledControllerTypes(linkOptions.disabledControllerTypes);
    linkOptions.disabledControllerTypes = disabledControllerTypes;

    std::future<bool> scanFuture;
    std::future<bool> startFuture;
    std::future<void> stopFuture;
    bool scanInFlight = false;
    bool startInFlight = false;
    bool rescanInFlight = false;
    bool stopInFlight = false;
    bool linkSyncPending = false;
    bool showLogsWindow = false;
    bool showPluginsWindow = false;
    bool showSettingsWindow = false;
    std::set<std::string> enabledControllers = loadEnabledControllers();
    std::unordered_map<std::string, std::string> controllerHostRoutes = loadControllerHostRoutes();

    auto launchSelectedStart = [&](std::set<std::string> selectedIds) {
        if (selectedIds.empty()) {
            return;
        }

        libera_link::LinkOptions options = linkOptions;
        options.virtualControllerRoutes.clear();
        options.virtualControllerRoutes.reserve(selectedIds.size());
        for (const auto& controllerId : selectedIds) {
            std::string hostId = linkOptions.virtualControllerHostId;
            const auto routeIt = controllerHostRoutes.find(controllerId);
            if (routeIt != controllerHostRoutes.end() && !routeIt->second.empty()) {
                hostId = routeIt->second;
            }
            if (hostId.empty()) {
                continue;
            }

            libera_link::VirtualControllerRoute route;
            route.controllerId = controllerId;
            route.hostId = hostId;
            route.options = linkOptions.virtualControllerHostOptions;
            options.virtualControllerRoutes.push_back(std::move(route));
        }
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

    auto launchScan = [&]() {
        const libera_link::LinkOptions options = linkOptions;
        scanFuture = std::async(std::launch::async, [&runtime, options] {
            return runtime.scan(options);
        });
        scanInFlight = true;
    };

    launchScan();

    while (app.beginFrame()) {
        if (scanFuture.valid() && scanFuture.wait_for(0ms) == std::future_status::ready) {
            (void)scanFuture.get();
            scanInFlight = false;
            linkSyncPending = true;
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
        const auto availableVirtualControllerHosts = libera_link::virtual_controller::availableVirtualControllerHosts();
        const auto availableControllerManagers = libera::System::availableControllerManagers();
        auto requestDiscoverySettingsSync = [&]() {
            linkOptions.disabledControllerTypes = disabledControllerTypes;
            saveDisabledControllerTypes(disabledControllerTypes);
            const bool linkRunning = snapshot.state == libera_link::RuntimeState::Running ||
                                       snapshot.state == libera_link::RuntimeState::StopRequested;
            if (!scanInFlight && !startInFlight && !stopInFlight && !rescanInFlight) {
                if (linkRunning) {
                    rescanInFlight = true;
                    linkSyncPending = false;
                } else {
                    linkSyncPending = false;
                    launchScan();
                }
            } else {
                linkSyncPending = true;
                if (startInFlight) {
                    runtime.requestStop();
                }
            }
        };
        const auto selectedVirtualControllerHostIt = std::find_if(
            availableVirtualControllerHosts.begin(), availableVirtualControllerHosts.end(),
            [&](const libera_link::virtual_controller::VirtualControllerHostInfo& info) {
                return info.id == linkOptions.virtualControllerHostId;
            });
        if (linkOptions.virtualControllerHostId.empty() || selectedVirtualControllerHostIt == availableVirtualControllerHosts.end()) {
            if (const auto defaultVirtualControllerHost = libera_link::virtual_controller::defaultVirtualControllerHost()) {
                linkOptions.virtualControllerHostId = defaultVirtualControllerHost->id;
            } else {
                linkOptions.virtualControllerHostId.clear();
            }
        }
        bool controllerHostRoutesChanged = false;
        for (auto it = controllerHostRoutes.begin(); it != controllerHostRoutes.end();) {
            const auto hostIt = std::find_if(
                availableVirtualControllerHosts.begin(), availableVirtualControllerHosts.end(),
                [&](const libera_link::virtual_controller::VirtualControllerHostInfo& info) {
                    return info.id == it->second;
                });
            if (hostIt == availableVirtualControllerHosts.end()) {
                it = controllerHostRoutes.erase(it);
                controllerHostRoutesChanged = true;
            } else {
                ++it;
            }
        }
        if (controllerHostRoutesChanged) {
            saveControllerHostRoutes(controllerHostRoutes);
        }

        if (snapshot.hasDiscoveryResults) {
            std::set<std::string> linkableControllerIds;
            for (const auto& controller : snapshot.discovered) {
                if (controller.linkable) {
                    linkableControllerIds.insert(controller.id);
                }
            }

            bool enabledControllersChanged = false;
            for (auto it = enabledControllers.begin(); it != enabledControllers.end();) {
                if (linkableControllerIds.find(*it) == linkableControllerIds.end()) {
                    it = enabledControllers.erase(it);
                    enabledControllersChanged = true;
                } else {
                    ++it;
                }
            }
            if (enabledControllersChanged) {
                saveEnabledControllers(enabledControllers);
            }
        }

        std::set<std::string> activeControllerIds;
        std::unordered_map<std::string, const libera_link::EndpointSnapshot*> endpointByControllerId;
        endpointByControllerId.reserve(snapshot.endpoints.size());
        for (const auto& endpoint : snapshot.endpoints) {
            activeControllerIds.insert(endpoint.id);
            endpointByControllerId.emplace(endpoint.id, &endpoint);
        }

        auto selectedLinkableIds = [&]() {
            std::set<std::string> selectedIds;
            for (const auto& controller : snapshot.discovered) {
                if (controller.linkable && enabledControllers.count(controller.id) > 0) {
                    selectedIds.insert(controller.id);
                }
            }
            return selectedIds;
        };

        auto selectedLinkableCount = [&]() -> std::size_t {
            std::size_t count = 0;
            for (const auto& controller : snapshot.discovered) {
                if (controller.linkable && enabledControllers.count(controller.id) > 0) {
                    ++count;
                }
            }
            return count;
        };

        auto linkableCount = [&]() -> std::size_t {
            return static_cast<std::size_t>(std::count_if(
                snapshot.discovered.begin(), snapshot.discovered.end(),
                [](const libera_link::DiscoveredControllerSnapshot& controller) {
                    return controller.linkable;
                }));
        };

        auto virtualControllerHostInfoForId = [&](const std::string& hostId) {
            return std::find_if(
                availableVirtualControllerHosts.begin(), availableVirtualControllerHosts.end(),
                [&](const libera_link::virtual_controller::VirtualControllerHostInfo& info) {
                    return info.id == hostId;
                });
        };

        auto virtualControllerHostDisplayName = [&](const std::string& hostId) {
            const auto it = std::find_if(
                availableVirtualControllerHosts.begin(), availableVirtualControllerHosts.end(),
                [&](const libera_link::virtual_controller::VirtualControllerHostInfo& info) {
                    return info.id == hostId;
                });
            if (it != availableVirtualControllerHosts.end()) {
                return it->displayName;
            }
            if (!hostId.empty()) {
                return hostId;
            }
            return std::string("Virtual Controller");
        };

        auto configuredVirtualControllerHostIdForController = [&](const std::string& controllerId) {
            const auto routeIt = controllerHostRoutes.find(controllerId);
            if (routeIt != controllerHostRoutes.end() && !routeIt->second.empty()) {
                return routeIt->second;
            }
            return linkOptions.virtualControllerHostId;
        };

        auto configuredVirtualControllerNameForController = [&](const std::string& controllerId) {
            return virtualControllerHostDisplayName(
                configuredVirtualControllerHostIdForController(controllerId));
        };

        const std::string configuredVirtualControllerName =
            virtualControllerHostDisplayName(linkOptions.virtualControllerHostId);

        const auto runtimeLabel = libera_link::runtimeStateLabel(snapshot.state);
        const ImVec4 runtimeColor = statusColor(snapshot.state);

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Libera Link", nullptr,
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
            (ImGui::GetTextLineHeightWithSpacing() * (showTopError ? 2.8f : 1.9f)) +
            12.0f;
        ImGui::BeginChild("LinkOverview", ImVec2(0.0f, overviewHeight), true, ImGuiWindowFlags_NoScrollbar);

        if (scanInFlight) {
            ImGui::BeginDisabled();
            ImGui::Button("Scanning...", ImVec2(140.0f, 0.0f));
            ImGui::EndDisabled();
        } else if (rescanInFlight) {
            ImGui::BeginDisabled();
            ImGui::Button("Rescanning...", ImVec2(140.0f, 0.0f));
            ImGui::EndDisabled();
        } else {
            const bool linkRunning = snapshot.state == libera_link::RuntimeState::Running ||
                                       snapshot.state == libera_link::RuntimeState::StopRequested;
            const char* scanButtonLabel = "RESCAN";
            const bool scanActionDisabled = startInFlight || stopInFlight;
            if (scanActionDisabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(scanButtonLabel, ImVec2(140.0f, 0.0f))) {
                if (linkRunning) {
                    rescanInFlight = true;
                    linkSyncPending = false;
                } else {
                    launchScan();
                }
            }
            if (scanActionDisabled) {
                ImGui::EndDisabled();
            }
        }

        ImGui::SameLine();
        const bool startAllDisabled =
            scanInFlight || startInFlight || stopInFlight || rescanInFlight || linkableCount() == 0;
        if (startAllDisabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("START ALL", ImVec2(140.0f, 0.0f))) {
            enabledControllers.clear();
            for (const auto& controller : snapshot.discovered) {
                if (controller.linkable) {
                    enabledControllers.insert(controller.id);
                }
            }
            saveEnabledControllers(enabledControllers);
            linkSyncPending = true;
        }
        if (startAllDisabled) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FK_COG "  Settings", ImVec2(140.0f, 0.0f))) {
            showSettingsWindow = true;
        }

        ImGui::Spacing();
        ImGui::TextColored(runtimeColor, "%s", runtimeLabel);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", snapshot.statusMessage.c_str());

        if (!snapshot.lastError.empty() && snapshot.state == libera_link::RuntimeState::Failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.34f, 0.34f, 1.0f), "%s", snapshot.lastError.c_str());
        } else {
            ImGui::TextDisabled("%zu linkable controllers | %zu enabled | %zu active endpoints",
                                linkableCount(),
                                selectedLinkableCount(),
                                snapshot.startedEndpoints);
        }
        ImGui::EndChild();

        ImGui::Spacing();

        ImGui::BeginChild("DetectedDacsPanel", ImVec2(0.0f, 0.0f), true);

        if (!snapshot.discovered.empty()) {
            ImGui::BeginChild("ControllerWireList",
                              ImVec2(0.0f, ImGui::GetContentRegionAvail().y),
                              false);
            const ImVec2 itemSpacing = ImGui::GetStyle().ItemSpacing;
            constexpr float controllerRowGapY = 2.0f;
            constexpr float controllerRowPadY = 2.0f;
            constexpr float controllerNodeHeight = 40.0f;
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing, ImVec2(itemSpacing.x, controllerRowGapY));
            for (const auto& controller : snapshot.discovered) {
                ImGui::PushID(controller.id.c_str());

                const auto endpointIt = endpointByControllerId.find(controller.id);
                const libera_link::EndpointSnapshot* endpoint =
                    endpointIt != endpointByControllerId.end() ? endpointIt->second : nullptr;
                bool selected = enabledControllers.count(controller.id) > 0;
                const bool disabled = !controller.linkable;
                const float rowWidth = std::max(ImGui::GetContentRegionAvail().x - 4.0f, 420.0f);
                const float rowHeight = controllerNodeHeight + (controllerRowPadY * 2.0f);
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + rowHeight);
                const float innerPadX = 4.0f;
                const float nodeHeight = controllerNodeHeight;
                const float centerY = rowMin.y + controllerRowPadY + (nodeHeight * 0.5f);
                float hostWidth = std::clamp(rowWidth * 0.24f, 160.0f, 260.0f);
                float controllerWidth = std::clamp(rowWidth * 0.32f, 210.0f, 360.0f);
                const float maxNodeWidth = rowWidth - (innerPadX * 2.0f) - 96.0f;
                if (hostWidth + controllerWidth > maxNodeWidth) {
                    const float scale = std::max(0.65f, maxNodeWidth / std::max(1.0f, hostWidth + controllerWidth));
                    hostWidth *= scale;
                    controllerWidth *= scale;
                }

                const ImVec2 hostMin(rowMin.x + innerPadX, centerY - nodeHeight * 0.5f);
                const ImVec2 hostMax(hostMin.x + hostWidth, hostMin.y + nodeHeight);
                const ImVec2 controllerMax(rowMax.x - innerPadX, centerY + nodeHeight * 0.5f);
                const ImVec2 controllerMin(controllerMax.x - controllerWidth, centerY - nodeHeight * 0.5f);
                const ImVec2 wireStart(hostMax.x, centerY);
                const ImVec2 wireEnd(controllerMin.x, centerY);

                ImGui::InvisibleButton("wire-row", ImVec2(rowWidth, rowHeight));
                const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                const bool hostSelectorHovered =
                    rowHovered && ImGui::IsMouseHoveringRect(hostMin, hostMax);
                const bool controllerHovered =
                    rowHovered && ImGui::IsMouseHoveringRect(controllerMin, controllerMax);
                const bool rowClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !disabled;
                const bool hostSelectorClicked =
                    rowClicked && hostSelectorHovered && !availableVirtualControllerHosts.empty();
                if (hostSelectorClicked) {
                    ImGui::OpenPopup("host-route-popup");
                }

                const bool toggled = rowClicked && !hostSelectorClicked;
                if (toggled) {
                    selected = !selected;
                    if (selected) {
                        enabledControllers.insert(controller.id);
                    } else {
                        enabledControllers.erase(controller.id);
                    }
                    saveEnabledControllers(enabledControllers);
                    linkSyncPending = true;
                    if (startInFlight) {
                        runtime.requestStop();
                    }
                }

                const bool active = endpoint != nullptr;
                const bool pendingSelectionChange =
                    controller.linkable && selected && !active &&
                    (startInFlight || stopInFlight || linkSyncPending || rescanInFlight);
                const bool hostOnline =
                    active && snapshot.state == libera_link::RuntimeState::Running;
                const bool hostFailed =
                    selected && !active && snapshot.state == libera_link::RuntimeState::Failed;
                const bool controllerOnline = controller.linkable;
                const bool dataFlowing =
                    hostOnline && endpoint &&
                    endpoint->stats.receivedPoints > 0 &&
                    endpoint->stats.observedInputPointRate > 0;

                const ImU32 hostLight = hostOnline
                    ? IM_COL32(65, 234, 130, 255)
                    : hostFailed
                          ? IM_COL32(236, 73, 66, 255)
                          : pendingSelectionChange
                                ? IM_COL32(240, 188, 65, 255)
                                : selected
                                      ? IM_COL32(88, 164, 255, 255)
                                      : IM_COL32(95, 101, 112, 255);
                const ImU32 controllerLight = active
                    ? IM_COL32(65, 234, 130, 255)
                    : !controllerOnline
                          ? IM_COL32(236, 73, 66, 255)
                          : selected
                                ? IM_COL32(88, 164, 255, 255)
                                : IM_COL32(95, 101, 112, 255);

                const ImU32 hostFill = hostOnline
                    ? IM_COL32(20, 48, 39, 245)
                    : hostFailed
                          ? IM_COL32(58, 27, 28, 245)
                          : selected
                                ? IM_COL32(22, 36, 54, 245)
                                : IM_COL32(27, 31, 38, 235);
                const ImU32 controllerFill = active
                    ? IM_COL32(20, 48, 39, 245)
                    : !controllerOnline
                          ? IM_COL32(58, 27, 28, 245)
                          : selected
                                ? IM_COL32(22, 36, 54, 245)
                                : IM_COL32(27, 31, 38, 235);
                const ImU32 activeBorder = IM_COL32(86, 210, 142, 230);
                const ImU32 idleBorder = IM_COL32(82, 98, 118, 190);
                const ImU32 textColor = disabled
                    ? IM_COL32(145, 150, 158, 210)
                    : IM_COL32(228, 234, 240, 255);
                const ImU32 secondaryTextColor = disabled
                    ? IM_COL32(118, 125, 136, 205)
                    : IM_COL32(166, 179, 192, 235);

                const auto configuredRouteIt = controllerHostRoutes.find(controller.id);
                const bool usingDefaultVirtualController =
                    configuredRouteIt == controllerHostRoutes.end() || configuredRouteIt->second.empty();
                const std::string configuredControllerHostId =
                    configuredVirtualControllerHostIdForController(controller.id);
                const auto configuredControllerHostIt =
                    virtualControllerHostInfoForId(configuredControllerHostId);
                const std::string hostLabel = endpoint && !endpoint->virtualControllerHostDisplayName.empty()
                    ? endpoint->virtualControllerHostDisplayName
                    : configuredVirtualControllerNameForController(controller.id);
                std::string hostSecondaryLabel;
                if (endpoint && !endpoint->virtualControllerEndpointLabel.empty() &&
                    endpoint->virtualControllerEndpointLabel != hostLabel) {
                    hostSecondaryLabel = endpoint->virtualControllerEndpointLabel;
                } else if (endpoint && !endpoint->virtualControllerEndpointAddress.empty() &&
                           endpoint->virtualControllerEndpointPort > 0) {
                    hostSecondaryLabel =
                        endpoint->virtualControllerEndpointAddress + ":" +
                        std::to_string(endpoint->virtualControllerEndpointPort);
                }

                auto drawVirtualControllerTooltip = [&]() {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s", hostLabel.c_str());
                    ImGui::TextDisabled("Route: %s",
                                        usingDefaultVirtualController ? "Default" : "Per-controller");
                    if (configuredControllerHostIt != availableVirtualControllerHosts.end()) {
                        ImGui::TextDisabled("ID: %s", configuredControllerHostIt->id.c_str());
                        if (!configuredControllerHostIt->description.empty()) {
                            ImGui::TextWrapped("%s", configuredControllerHostIt->description.c_str());
                        }
                    } else if (!configuredControllerHostId.empty()) {
                        ImGui::TextDisabled("ID: %s", configuredControllerHostId.c_str());
                    }
                    if (endpoint) {
                        ImGui::Separator();
                        if (!endpoint->virtualControllerEndpointLabel.empty()) {
                            ImGui::Text("Presented as: %s",
                                        endpoint->virtualControllerEndpointLabel.c_str());
                        }
                        if (!endpoint->virtualControllerEndpointKind.empty() ||
                            !endpoint->virtualControllerEndpointValue.empty()) {
                            ImGui::Text("Endpoint: %s %s",
                                        endpoint->virtualControllerEndpointKind.c_str(),
                                        endpoint->virtualControllerEndpointValue.c_str());
                        }
                        if (!endpoint->virtualControllerEndpointProtocol.empty()) {
                            ImGui::Text("Protocol: %s",
                                        endpoint->virtualControllerEndpointProtocol.c_str());
                        }
                        if (!endpoint->virtualControllerEndpointTransport.empty()) {
                            if (!endpoint->virtualControllerEndpointAddress.empty() &&
                                endpoint->virtualControllerEndpointPort > 0) {
                                ImGui::Text("Transport: %s %s:%u",
                                            endpoint->virtualControllerEndpointTransport.c_str(),
                                            endpoint->virtualControllerEndpointAddress.c_str(),
                                            endpoint->virtualControllerEndpointPort);
                            } else {
                                ImGui::Text("Transport: %s",
                                            endpoint->virtualControllerEndpointTransport.c_str());
                            }
                        }
                        if (endpoint->virtualControllerEndpointChannels > 0) {
                            ImGui::Text("Channels: %u",
                                        endpoint->virtualControllerEndpointChannels);
                        }
                        ImGui::Text("Input: %u pps",
                                    endpoint->stats.observedInputPointRate);
                        ImGui::Text("Received: %llu points",
                                    static_cast<unsigned long long>(endpoint->stats.receivedPoints));
                    }
                    ImGui::EndTooltip();
                };

                auto drawControllerTooltip = [&]() {
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
                        if (!endpoint->virtualControllerEndpointLabel.empty()) {
                            ImGui::Text("Endpoint: %s", endpoint->virtualControllerEndpointLabel.c_str());
                        }
                        if (!endpoint->virtualControllerEndpointProtocol.empty()) {
                            ImGui::Text("Protocol: %s", endpoint->virtualControllerEndpointProtocol.c_str());
                        }
                        if (!endpoint->virtualControllerEndpointTransport.empty()) {
                            if (!endpoint->virtualControllerEndpointAddress.empty() &&
                                endpoint->virtualControllerEndpointPort > 0) {
                                ImGui::Text("Transport: %s %s:%u",
                                            endpoint->virtualControllerEndpointTransport.c_str(),
                                            endpoint->virtualControllerEndpointAddress.c_str(),
                                            endpoint->virtualControllerEndpointPort);
                            } else {
                                ImGui::Text("Transport: %s",
                                            endpoint->virtualControllerEndpointTransport.c_str());
                            }
                        }
                        if (!endpoint->virtualControllerHostDisplayName.empty()) {
                            ImGui::Text("Virtual Controller: %s", endpoint->virtualControllerHostDisplayName.c_str());
                        }
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

                ImDrawList* drawList = ImGui::GetWindowDrawList();

                if (ImGui::BeginPopup("host-route-popup")) {
                    const std::string defaultLabel =
                        "Default: " + configuredVirtualControllerName;
                    if (ImGui::Selectable(defaultLabel.c_str(), usingDefaultVirtualController)) {
                        controllerHostRoutes.erase(controller.id);
                        saveControllerHostRoutes(controllerHostRoutes);
                        linkSyncPending = true;
                        if (startInFlight) {
                            runtime.requestStop();
                        }
                    }
                    if (usingDefaultVirtualController) {
                        ImGui::SetItemDefaultFocus();
                    }
                    ImGui::Separator();
                    for (const auto& virtualControllerHost : availableVirtualControllerHosts) {
                        const bool selectedHost =
                            !usingDefaultVirtualController &&
                            configuredControllerHostId == virtualControllerHost.id;
                        if (ImGui::Selectable(virtualControllerHost.displayName.c_str(), selectedHost)) {
                            if (virtualControllerHost.id == linkOptions.virtualControllerHostId) {
                                controllerHostRoutes.erase(controller.id);
                            } else {
                                controllerHostRoutes[controller.id] = virtualControllerHost.id;
                            }
                            saveControllerHostRoutes(controllerHostRoutes);
                            linkSyncPending = true;
                            if (startInFlight) {
                                runtime.requestStop();
                            }
                        }
                        if (!virtualControllerHost.description.empty() && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", virtualControllerHost.description.c_str());
                        }
                        if (selectedHost) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndPopup();
                }

                drawLinkNode(drawList,
                             hostMin,
                             hostMax,
                             hostLabel,
                             hostFill,
                             hostSelectorHovered ? IM_COL32(128, 181, 222, 230)
                                                 : hostOnline ? activeBorder : idleBorder,
                             textColor,
                             hostLight,
                             hostOnline,
                             hostSecondaryLabel,
                             secondaryTextColor);
                drawVirtualWire(drawList,
                                wireStart,
                                wireEnd,
                                hostOnline,
                                dataFlowing,
                                !hostOnline,
                                !controllerOnline);
                drawLinkNode(drawList,
                             controllerMin,
                             controllerMax,
                             controller.label,
                             controllerFill,
                             active ? activeBorder : idleBorder,
                             textColor,
                             controllerLight,
                             active);

                if (!ImGui::IsPopupOpen("host-route-popup")) {
                    if (controllerHovered) {
                        drawControllerTooltip();
                    } else if (hostSelectorHovered) {
                        drawVirtualControllerTooltip();
                    }
                }

                ImGui::PopID();
            }
            ImGui::PopStyleVar();
            ImGui::EndChild();
        }

        if (snapshot.discovered.empty()) {
            if (snapshot.hasDiscoveryResults) {
                ImGui::TextWrapped("No controllers were found during the last scan. Check connections or plugins, then rescan.");
            } else {
                ImGui::TextWrapped("No scan results yet. Click RESCAN to discover controllers, then enable the ones you want to link.");
            }
        } else if (linkableCount() == 0) {
            ImGui::TextWrapped("Controllers were discovered, but none of them are currently linkable.");
        }
        ImGui::EndChild();

        ImGui::End();

        if (showSettingsWindow) {
            ImGui::SetNextWindowSize(ImVec2(620.0f, 500.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(ICON_FK_COG "  Settings", &showSettingsWindow, ImGuiWindowFlags_NoCollapse)) {
                drawSectionTitle(app, "Virtual Controller");
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Default");
                ImGui::SameLine();
                if (availableVirtualControllerHosts.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.34f, 0.34f, 1.0f), "%s", "None registered");
                } else {
                    const bool virtualControllerSelectionDisabled =
                        startInFlight || stopInFlight || scanInFlight || rescanInFlight;
                    if (virtualControllerSelectionDisabled) {
                        ImGui::BeginDisabled();
                    }

                    const auto activeVirtualControllerHostIt =
                        virtualControllerHostInfoForId(linkOptions.virtualControllerHostId);
                    const std::string preview =
                        activeVirtualControllerHostIt != availableVirtualControllerHosts.end()
                            ? activeVirtualControllerHostIt->displayName
                            : linkOptions.virtualControllerHostId;
                    ImGui::SetNextItemWidth(260.0f);
                    if (ImGui::BeginCombo("##settings-virtual-controller-host", preview.c_str())) {
                        for (const auto& virtualControllerHost : availableVirtualControllerHosts) {
                            const bool selected =
                                virtualControllerHost.id == linkOptions.virtualControllerHostId;
                            if (ImGui::Selectable(virtualControllerHost.displayName.c_str(), selected)) {
                                linkOptions.virtualControllerHostId = virtualControllerHost.id;
                                saveDefaultVirtualControllerHostId(linkOptions.virtualControllerHostId);
                                linkSyncPending = true;
                            }
                            if (!virtualControllerHost.description.empty() && ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", virtualControllerHost.description.c_str());
                            }
                            if (selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    if (virtualControllerSelectionDisabled) {
                        ImGui::EndDisabled();
                    }

                    if (activeVirtualControllerHostIt != availableVirtualControllerHosts.end() &&
                        !activeVirtualControllerHostIt->description.empty()) {
                        ImGui::TextWrapped("%s", activeVirtualControllerHostIt->description.c_str());
                    }
                }

                ImGui::Spacing();
                drawSectionTitle(app, "Windows");
                if (ImGui::Button("Logs", ImVec2(120.0f, 0.0f))) {
                    showLogsWindow = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FK_PLUS_CIRCLE "  Plugins", ImVec2(140.0f, 0.0f))) {
                    showPluginsWindow = true;
                }

                ImGui::Separator();
                ImGui::Spacing();
                drawSectionTitle(app, "Controller Discovery");

                const bool discoverySettingsLocked =
                    scanInFlight || startInFlight || stopInFlight || rescanInFlight;

                if (!disabledControllerTypes.empty()) {
                    if (discoverySettingsLocked) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Button(ICON_FK_CHECK "  Enable All", ImVec2(150.0f, 0.0f))) {
                        disabledControllerTypes.clear();
                        requestDiscoverySettingsSync();
                    }
                    if (discoverySettingsLocked) {
                        ImGui::EndDisabled();
                    }
                }

                ImGui::Spacing();
                ImGui::BeginChild("ControllerDiscoverySettings",
                                  ImVec2(0.0f, 0.0f),
                                  true,
                                  ImGuiWindowFlags_AlwaysVerticalScrollbar);

                if (availableControllerManagers.empty()) {
                    ImGui::TextDisabled("No controller managers registered.");
                }

                for (const auto& manager : availableControllerManagers) {
                    if (manager.type.empty()) {
                        continue;
                    }
                    ImGui::PushID(manager.type.c_str());

                    bool enabled = disabledControllerTypes.find(manager.type) ==
                                   disabledControllerTypes.end();
                    if (discoverySettingsLocked) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Checkbox("##enabled", &enabled)) {
                        if (enabled) {
                            disabledControllerTypes.erase(manager.type);
                        } else {
                            disabledControllerTypes.insert(manager.type);
                        }
                        requestDiscoverySettingsSync();
                    }
                    if (discoverySettingsLocked) {
                        ImGui::EndDisabled();
                    }

                    ImGui::SameLine();
                    const std::string label =
                        manager.displayName.empty() ? manager.type : manager.displayName;
                    ImGui::TextUnformatted(label.c_str());
                    if (!manager.description.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", manager.description.c_str());
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", manager.type.c_str());

                    ImGui::PopID();
                }

                ImGui::EndChild();
            }
            ImGui::End();
        }

        if (showLogsWindow) {
            ImGui::SetNextWindowSize(ImVec2(900.0f, 420.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Link Logs", &showLogsWindow, ImGuiWindowFlags_NoCollapse)) {
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

        const auto targetControllerIds = selectedLinkableIds();
        const bool linkRunning = snapshot.state == libera_link::RuntimeState::Running ||
                                   snapshot.state == libera_link::RuntimeState::StopRequested;
        const bool linkSelectionMatches = activeControllerIds == targetControllerIds;
        const bool linkVirtualControllerMatches = !linkRunning || !linkSyncPending;

        if (!scanInFlight && !startInFlight && !stopInFlight) {
            if (rescanInFlight) {
                if (linkRunning) {
                    launchStop();
                } else if (!targetControllerIds.empty()) {
                    linkSyncPending = false;
                    launchSelectedStart(targetControllerIds);
                } else {
                    rescanInFlight = false;
                }
            } else if (linkSyncPending) {
                if (linkRunning) {
                    if (!linkSelectionMatches || !linkVirtualControllerMatches) {
                        launchStop();
                    } else {
                        linkSyncPending = false;
                    }
                } else {
                    if (!targetControllerIds.empty()) {
                        linkSyncPending = false;
                        launchSelectedStart(targetControllerIds);
                    } else {
                        linkSyncPending = false;
                    }
                }
            }
        }

        libera::ui::DrawPluginsWindow(&showPluginsWindow);
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
