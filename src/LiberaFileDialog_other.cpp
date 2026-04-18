#include "LiberaFileDialog.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <sstream>
#elif !defined(__APPLE__)
#include <cstdio>
#include <sstream>
#endif

namespace libera::ui {

#ifdef _WIN32
std::string OpenFileDialog(const char* title,
                           const std::vector<std::string>& extensions) {
    char file[MAX_PATH] = {0};

    std::string filter;
    if (!extensions.empty()) {
        std::ostringstream label;
        label << "Allowed files (";
        for (std::size_t i = 0; i < extensions.size(); ++i) {
            if (i != 0) {
                label << ";";
            }
            label << "*." << extensions[i];
        }
        label << ")";
        filter = label.str();
        filter.push_back('\0');
        for (std::size_t i = 0; i < extensions.size(); ++i) {
            if (i != 0) {
                filter += ";";
            }
            filter += "*." + extensions[i];
        }
        filter.push_back('\0');
        filter += "All files\0*.*\0";
    } else {
        filter = std::string("All files\0*.*\0", 14);
    }

    OPENFILENAMEA dialog = {0};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = file;
    dialog.nMaxFile = sizeof(file);
    dialog.lpstrFilter = filter.c_str();
    dialog.nFilterIndex = 1;
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&dialog)) {
        return {};
    }
    return file;
}
#elif !defined(__APPLE__)
std::string OpenFileDialog(const char* title,
                           const std::vector<std::string>& extensions) {
    std::ostringstream command;
    command << "zenity --file-selection";
    if (title && *title) {
        command << " --title=\"" << title << "\"";
    }
    if (!extensions.empty()) {
        command << " --file-filter=\"";
        for (std::size_t i = 0; i < extensions.size(); ++i) {
            if (i != 0) {
                command << " ";
            }
            command << "*." << extensions[i];
        }
        command << "\"";
    }
    command << " 2>/dev/null";

    FILE* process = popen(command.str().c_str(), "r");
    if (!process) {
        return {};
    }

    std::string output;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), process)) {
        output += buffer;
    }
    pclose(process);

    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}
#endif

} // namespace libera::ui
