#pragma once

#include <filesystem>
#include <string>

namespace libera_link {

std::filesystem::path avbSettingsPath();
// Read the whole file before applying it, so a damaged setting cannot leave
// the backend with a partly restored set of interfaces.
bool loadAvbSettings(const std::filesystem::path& path, std::string& error);
bool saveAvbSettings(const std::filesystem::path& path, std::string& error);

} // namespace libera_link
