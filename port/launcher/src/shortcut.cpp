#include "shortcut.h"

#include "subprocess.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <cctype>
#include <fstream>
#endif

bool create_desktop_shortcut(const std::filesystem::path& target, const std::filesystem::path& folder, const std::filesystem::path& icon, const std::string& name,
                             std::string& error) {
#ifdef _WIN32
    (void)icon;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    PWSTR desktop = nullptr;
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) &&
              SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link))) &&
              SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link->SetWorkingDirectory(folder.c_str())) &&
              SUCCEEDED(link->SetIconLocation(target.c_str(), 0)) && SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)));
    if (ok) {
        std::filesystem::path shortcut = std::filesystem::path(desktop) / path_from(name + ".lnk");
        ok = SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
    }
    if (!ok) {
        error = "Windows " + std::to_string(static_cast<unsigned long>(GetLastError()));
    }
    if (file) {
        file->Release();
    }
    if (link) {
        link->Release();
    }
    if (desktop) {
        CoTaskMemFree(desktop);
    }
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return ok;
#else
    std::string home = environment_value("HOME");
    std::string data = environment_value("XDG_DATA_HOME");
    std::filesystem::path applications = (data.empty() ? path_from(home) / ".local" / "share" : path_from(data)) / "applications";
    std::string desktop = capture("xdg-user-dir", {"DESKTOP"});
    while (!desktop.empty() && std::isspace(static_cast<unsigned char>(desktop.back()))) {
        desktop.pop_back();
    }
    std::filesystem::path desktop_folder = desktop.empty() ? path_from(home) / "Desktop" : path_from(desktop);
    std::string file_name;
    for (char ch : name) {
        file_name += ch == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    file_name += ".desktop";
    std::string entry = "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=\"" + utf8_of(target) + "\"\nPath=" + utf8_of(folder) +
                        "\nTerminal=false\nCategories=Game;\n";
    std::error_code missing;
    if (std::filesystem::exists(icon, missing)) {
        entry += "Icon=" + utf8_of(icon) + "\n";
    }
    bool written = false;
    for (const std::filesystem::path& place : {applications, desktop_folder}) {
        std::error_code failure;
        if (place == desktop_folder && !std::filesystem::is_directory(place, failure)) {
            continue;
        }
        std::filesystem::create_directories(place, failure);
        std::filesystem::path shortcut = place / file_name;
        std::ofstream out(shortcut, std::ios::binary | std::ios::trunc);
        out << entry;
        out.close();
        if (!out) {
            error = utf8_of(shortcut);
            return false;
        }
        std::filesystem::permissions(shortcut, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add, failure);
        written = true;
    }
    return written;
#endif
}
