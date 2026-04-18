// LiberaPluginsWindow — ImGui panel for managing Libera plugins.

#pragma once

#include <string>

namespace libera::ui {

void DrawPluginsWindow(bool* open, const std::string& userPluginDir);

} // namespace libera::ui
