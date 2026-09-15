#include "AvbSettings.hpp"
#include "LiberaPaths.hpp"

#include "libera/avb/AvbManager.hpp"

#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace libera_link {

std::filesystem::path avbSettingsPath() {
    return std::filesystem::path(settingsDirectory()) / "avb-settings.txt";
}

bool loadAvbSettings(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "Cannot read AVB settings: " + ec.message();
        return false;
    }
    if (!exists) {
        return true;
    }

    std::ifstream input(path);
    if (!input) {
        error = "Cannot open AVB settings: " + path.string();
        return false;
    }

    std::vector<libera::avb::AvbDeviceConfiguration> configs;
    std::vector<std::string> halfXYControllers;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        std::istringstream fields(line);
        fields >> std::ws;
        if (fields.eof() || fields.peek() == '#') {
            continue;
        }
        std::string kind;
        std::string id;
        bool valid = static_cast<bool>(fields >> kind >> std::quoted(id)) && !id.empty();
        if (valid && kind == "device") {
            std::int64_t pointRate = 0;
            valid = static_cast<bool>(fields >> pointRate) && pointRate > 0 &&
                pointRate <= std::numeric_limits<std::uint32_t>::max();
            if (valid) {
                configs.push_back({id, static_cast<std::uint32_t>(pointRate)});
            }
        } else if (valid && kind == "half-xy") {
            halfXYControllers.push_back(id);
        } else {
            valid = false;
        }
        fields >> std::ws;
        if (!valid || !fields.eof()) {
            error = "Invalid AVB setting on line " + std::to_string(lineNumber) + ".";
            return false;
        }
    }
    if (input.bad()) {
        error = "Cannot finish reading AVB settings: " + path.string();
        return false;
    }

    // Libera retains configurations for unplugged interfaces, so they can
    // reappear with the same point rate and bank settings after reconnecting.
    libera::avb::AvbManager::setConfiguredDevices(configs);
    libera::avb::AvbManager::setHalfXYOutputControllers(halfXYControllers);
    return true;
}

bool saveAvbSettings(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::ofstream output(path, std::ios::trunc);
    if (output) {
        output << "# Libera Link AVB interfaces and per-bank output settings\n";
        // Quoted IDs preserve spaces, quotes, and backslashes in audio UIDs.
        for (const auto& config : libera::avb::AvbManager::configuredDevices()) {
            output << "device " << std::quoted(config.deviceUid) << ' '
                   << config.preferredPointRate << '\n';
        }
        for (const auto& id : libera::avb::AvbManager::halfXYOutputControllers()) {
            output << "half-xy " << std::quoted(id) << '\n';
        }
        output.close();
    }
    if (!output) {
        error = "AVB settings could not be saved to " + path.string() + ".";
        return false;
    }
    return true;
}

} // namespace libera_link
