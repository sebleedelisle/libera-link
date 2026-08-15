#include "LiberaPluginsWindow.h"

#include "LiberaFileDialog.h"
#include "fonts/IconsForkAwesome.h"
#include "imgui.h"
#include "libera/gui/imgui/PluginManagementPanel.hpp"
#include "libera/plugin/PluginManagement.hpp"

#include <cstdlib>
#include <optional>
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

libera::gui::imgui::PluginPanelState& pluginPanelState() {
    static libera::gui::imgui::PluginPanelState state;
    return state;
}

std::optional<std::string> choosePluginFile() {
    const std::string picked = OpenFileDialog(
        "Choose a Libera plugin to install",
        {libera::plugin::platformPluginExtension()});
    if (picked.empty()) {
        return std::nullopt;
    }
    return picked;
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

void DrawPluginsWindow(bool* open) {
    if (!open || !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FK_PLUS_CIRCLE "  Plugins", open)) {
        ImGui::End();
        return;
    }

    libera::gui::imgui::PluginPanelCallbacks callbacks;
    callbacks.choosePluginFile = choosePluginFile;
    callbacks.requestRestart = relaunchAndExit;
    libera::gui::imgui::DrawPluginManagementPanel(pluginPanelState(), callbacks);

    ImGui::End();
}

} // namespace libera::ui
