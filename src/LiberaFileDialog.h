// Native "open file" dialog. Returns the chosen path, or empty string if the
// user cancelled. `extensions` is a list of allowed file extensions without
// the leading dot (e.g. {"dylib"}). Pass an empty vector to allow any file.

#pragma once

#include <string>
#include <vector>

namespace libera::ui {

std::string OpenFileDialog(const char* title,
                           const std::vector<std::string>& extensions);

} // namespace libera::ui
