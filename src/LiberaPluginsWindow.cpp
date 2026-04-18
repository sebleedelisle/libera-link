#include "LiberaPluginsWindow.h"

#include "LiberaFileDialog.h"
#include "fonts/IconsForkAwesome.h"
#include "imgui.h"
#include "libera/plugin/PluginRegistry.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace libera::ui {
namespace {

const char* stateLabel(libera::plugin::PluginState state) {
    using State = libera::plugin::PluginState;
    switch (state) {
        case State::Loaded:           return "Loaded";
        case State::NotAPlugin:       return "Not a Libera plugin";
        case State::FailedLoad:       return "Failed to load";
        case State::FailedValidation: return "Validation failed";
        case State::FailedBackend:    return "Backend init failed";
    }
    return "?";
}

ImU32 stateColor(libera::plugin::PluginState state) {
    using State = libera::plugin::PluginState;
    switch (state) {
        case State::Loaded:           return IM_COL32(60, 200, 90, 255);
        case State::NotAPlugin:       return IM_COL32(150, 150, 150, 255);
        case State::FailedLoad:
        case State::FailedValidation:
        case State::FailedBackend:    return IM_COL32(230, 90, 90, 255);
    }
    return IM_COL32(150, 150, 150, 255);
}

std::string formatTime(std::chrono::system_clock::time_point timePoint) {
    const auto raw = std::chrono::system_clock::to_time_t(timePoint);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &raw);
#else
    localtime_r(&raw, &localTime);
#endif

    char buffer[32];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%02d:%02d:%02d",
                  localTime.tm_hour,
                  localTime.tm_min,
                  localTime.tm_sec);
    return buffer;
}

struct UiState {
    std::string lastInstallMessage;
    bool lastInstallOk = false;
    std::string lastRemoveMessage;
    bool restartHintVisible = false;
};

#ifdef _WIN32
constexpr const char* kPluginExtension = "dll";
#elif defined(__APPLE__)
constexpr const char* kPluginExtension = "dylib";
#else
constexpr const char* kPluginExtension = "so";
#endif

UiState& uiState() {
    static UiState state;
    return state;
}

void relaunchAndExit() {
#ifdef __APPLE__
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        std::exit(0);
    }

    std::string executablePath(buffer);
    const auto contentsMacOs = executablePath.rfind("/Contents/MacOS/");
    const std::string target = (contentsMacOs != std::string::npos)
        ? executablePath.substr(0, contentsMacOs)
        : executablePath;
    const std::string command = "(sleep 0.4; open -n \"" + target + "\") &";
    std::system(command.c_str());
    std::exit(0);
#elif defined(_WIN32)
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    CreateProcessA(path,
                   nullptr,
                   nullptr,
                   nullptr,
                   FALSE,
                   DETACHED_PROCESS,
                   nullptr,
                   nullptr,
                   &startupInfo,
                   &processInfo);
    if (processInfo.hProcess) {
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
    }
    std::exit(0);
#else
    char buffer[4096];
    const ssize_t count = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (count <= 0) {
        std::exit(0);
    }

    buffer[count] = '\0';
    const std::string command = std::string("(sleep 0.4; \"") + buffer + "\") &";
    std::system(command.c_str());
    std::exit(0);
#endif
}

} // namespace

void DrawPluginsWindow(bool* open, const std::string& userPluginDir) {
    if (!open || !*open) {
        return;
    }

    auto& state = uiState();

    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FK_PLUS_CIRCLE "  Plugins", open)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("User plugin directory:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", userPluginDir.c_str());

    if (state.restartHintVisible) {
        if (ImGui::Button(ICON_FK_REFRESH "  Restart now", ImVec2(140.0f, 0.0f))) {
            relaunchAndExit();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 180, 70, 255));
        ImGui::TextWrapped(ICON_FK_EXCLAMATION_TRIANGLE
                           "  Restart the app for plugin changes to take effect.");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    if (ImGui::Button(ICON_FK_UPLOAD "  Install new plugin...", ImVec2(220.0f, 0.0f))) {
        const std::string picked = libera::ui::OpenFileDialog(
            "Choose a Libera plugin to install", {kPluginExtension});
        if (!picked.empty()) {
            const auto result = libera::plugin::installPluginFile(picked, userPluginDir);
            state.lastInstallOk = result.success;
            state.lastInstallMessage = result.message;
            if (result.success) {
                state.restartHintVisible = true;
                state.lastRemoveMessage.clear();
            }
        }
    }

    if (!state.lastInstallMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              state.lastInstallOk
                                  ? IM_COL32(120, 220, 120, 255)
                                  : IM_COL32(230, 120, 120, 255));
        ImGui::TextWrapped("%s", state.lastInstallMessage.c_str());
        ImGui::PopStyleColor();
    }

    if (!state.lastRemoveMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 120, 120, 255));
        ImGui::TextWrapped("%s", state.lastRemoveMessage.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const auto plugins = libera::plugin::PluginRegistry::instance().snapshot();
    if (plugins.empty()) {
        ImGui::TextDisabled("No plugins found.");
    }

    for (const auto& plugin : plugins) {
        ImGui::PushID(plugin.path.c_str());

        ImVec2 dotPosition = ImGui::GetCursorScreenPos();
        const float dotRadius = ImGui::GetTextLineHeight() * 0.45f;
        dotPosition.x += dotRadius;
        dotPosition.y += ImGui::GetTextLineHeight() * 0.5f;
        ImGui::GetWindowDrawList()->AddCircleFilled(dotPosition, dotRadius, stateColor(plugin.state));
        ImGui::Dummy(ImVec2(dotRadius * 2.0f + 6.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine();

        const std::string headerLabel =
            (!plugin.displayName.empty() ? plugin.displayName : plugin.filename) +
            "##hdr_" + plugin.path;
        const float removeButtonWidth = 110.0f;
        const bool expanded = ImGui::CollapsingHeader(
            headerLabel.c_str(), ImGuiTreeNodeFlags_AllowOverlap);

        ImGui::SameLine(ImGui::GetContentRegionMax().x - removeButtonWidth);
        if (ImGui::Button(ICON_FK_TRASH_O "  Remove", ImVec2(removeButtonWidth, 0.0f))) {
            std::string error;
            if (libera::plugin::removePluginFile(plugin.path, &error)) {
                state.lastRemoveMessage.clear();
                state.restartHintVisible = true;
            } else {
                state.lastRemoveMessage = "Remove failed: " + error;
            }
        }

        if (expanded) {
            ImGui::Indent();

            ImGui::TextDisabled("State:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, stateColor(plugin.state));
            ImGui::TextUnformatted(stateLabel(plugin.state));
            ImGui::PopStyleColor();

            if (!plugin.typeName.empty()) {
                ImGui::TextDisabled("Type:");
                ImGui::SameLine();
                ImGui::TextUnformatted(plugin.typeName.c_str());
            }

            ImGui::TextDisabled("File:");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", plugin.path.c_str());

            if (plugin.loadError) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 120, 120, 255));
                ImGui::TextWrapped(ICON_FK_EXCLAMATION_TRIANGLE "  %s",
                                   plugin.loadError->c_str());
                ImGui::PopStyleColor();
            }

            if (!plugin.runtimeErrors.empty()) {
                ImGui::Spacing();
                if (ImGui::TreeNode("runtime_errors",
                                    "Runtime errors (%zu)",
                                    plugin.runtimeErrors.size())) {
                    ImGui::BeginChild("##runtime_errors", ImVec2(0.0f, 140.0f), true);
                    for (auto it = plugin.runtimeErrors.rbegin();
                         it != plugin.runtimeErrors.rend();
                         ++it) {
                        ImGui::TextDisabled("[%s]", formatTime(it->time).c_str());
                        ImGui::SameLine();
                        ImGui::TextUnformatted(it->code.c_str());
                        if (!it->message.empty()) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("- %s", it->message.c_str());
                        }
                    }
                    ImGui::EndChild();
                    ImGui::TreePop();
                }
            }

            ImGui::Unindent();
        }

        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace libera::ui
