#include "LiberaPaths.hpp"

#include "libera/System.hpp"
#include "libera/plugin/PluginManagement.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace libera_link {
namespace {

std::string envValue(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
}

std::filesystem::path defaultSettingsDirectory() {
    std::filesystem::path baseDir;

#ifdef _WIN32
    const auto appData = envValue("APPDATA");
    if (!appData.empty()) {
        baseDir = appData;
    } else {
        const auto userProfile = envValue("USERPROFILE");
        if (!userProfile.empty()) {
            baseDir = std::filesystem::path(userProfile) / "AppData" / "Roaming";
        }
    }
#elif defined(__APPLE__)
    const auto home = envValue("HOME");
    if (!home.empty()) {
        baseDir = std::filesystem::path(home) / "Library" / "Application Support";
    }
#else
    const auto xdgConfig = envValue("XDG_CONFIG_HOME");
    if (!xdgConfig.empty()) {
        baseDir = xdgConfig;
    } else {
        const auto home = envValue("HOME");
        if (!home.empty()) {
            baseDir = std::filesystem::path(home) / ".config";
        }
    }
#endif

    if (baseDir.empty()) {
        baseDir = std::filesystem::current_path();
    }

    return baseDir / "LiberaLink";
}

} // namespace

const std::string& settingsDirectory() {
    static const std::string path = [] {
        const auto dir = defaultSettingsDirectory();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }();
    return path;
}

const std::string& userPluginDirectory() {
    return libera::plugin::userPluginDirectory();
}

void configureLiberaPluginDirectories() {
    // Plugin loading now defaults to Libera's shared user plugin directory.
    // Keep this function as a no-op for existing Link startup code.
}

} // namespace libera_link
