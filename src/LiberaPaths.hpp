#pragma once

#include <string>

namespace idn_bridge {

const std::string& settingsDirectory();
const std::string& userPluginDirectory();
void configureLiberaPluginDirectories();

} // namespace idn_bridge
