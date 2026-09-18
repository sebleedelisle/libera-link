#include "LiberaFileDialog.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#elif !defined(__APPLE__)
#include <cstdio>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace libera::ui {

#ifdef _WIN32
namespace {

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8,
                                         MB_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr,
                                         0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        result.data(),
                        size);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8,
                                         WC_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        result.data(),
                        size,
                        nullptr,
                        nullptr);
    return result;
}

} // namespace

std::string OpenFileDialog(const char* title,
                           const std::vector<std::string>& extensions) {
    std::vector<wchar_t> file(32768, L'\0');

    std::wstring filter;
    if (!extensions.empty()) {
        filter += L"Allowed files (";
        for (std::size_t i = 0; i < extensions.size(); ++i) {
            if (i != 0) {
                filter += L";";
            }
            filter += L"*." + utf8ToWide(extensions[i]);
        }
        filter += L")";
        filter.push_back(L'\0');
        for (std::size_t i = 0; i < extensions.size(); ++i) {
            if (i != 0) {
                filter += L";";
            }
            filter += L"*." + utf8ToWide(extensions[i]);
        }
        filter.push_back(L'\0');
        filter += L"All files";
        filter.push_back(L'\0');
        filter += L"*.*";
        filter.push_back(L'\0');
    } else {
        filter = std::wstring(L"All files\0*.*\0", 14);
    }
    filter.push_back(L'\0');

    const std::wstring wideTitle = title ? utf8ToWide(title) : std::wstring{};

    OPENFILENAMEW dialog = {0};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrFilter = filter.c_str();
    dialog.nFilterIndex = 1;
    dialog.lpstrTitle = wideTitle.empty() ? nullptr : wideTitle.c_str();
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST |
                   OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        return {};
    }
    return wideToUtf8(file.data());
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

void OpenPath(const std::string& path) {
#ifdef _WIN32
    const auto widePath = utf8ToWide(path);
    if (widePath.empty()) return;
    ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const pid_t child = fork();
    if (child == 0) {
        const pid_t grandchild = fork();
        if (grandchild == 0) {
            execlp("xdg-open", "xdg-open", path.c_str(), nullptr);
        }
        _exit(127);
    }
    if (child > 0) waitpid(child, nullptr, 0);
#endif
}

} // namespace libera::ui
