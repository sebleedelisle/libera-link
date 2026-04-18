#include "LiberaPaths.hpp"

#include "libera/System.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace idn_bridge {
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

    return baseDir / "LiberaPortal";
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
    static const std::string path = [] {
        const auto dir = std::filesystem::path(settingsDirectory()) / "plugins";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }();
    return path;
}

void configureLiberaPluginDirectories() {
    static bool configured = false;
    if (configured) {
        return;
    }

    libera::System::addPluginDirectory(userPluginDirectory());
    configured = true;
}

} // namespace idn_bridge
