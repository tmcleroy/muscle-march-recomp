#include "toolchain.h"

#include <cstdlib>
#include <cstring>
#include <regex>

#include "subprocess.h"
#include "sha256.h"
#include "texts.h"

namespace {

enum class Packaging { Archive, SelfExtracting, Single };

struct Package {
    const char* program;
    const char* name;
    const char* url;
    const char* sha256;
    const char* file;
    Packaging packaging;
    const char* folder;
    const char* bin;
};

#ifdef _WIN32
constexpr Package kPackages[] = {
    {"nodtool", "nodtool 1.4.4", "https://github.com/encounter/nod/releases/download/v1.4.4/nodtool-windows-x86_64.exe",
     "fb3203a68a59fa19ba9de0aa2f8c339396b6e4c5f9274d0858d302074cdedf8b", "nodtool.exe", Packaging::Single, "nodtool", "nodtool"},
    {"python", "Python 3.12.10", "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
     "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3", "python-3.12.10-embed-amd64.zip", Packaging::Archive, "python", "python"},
    {"cmake", "CMake 4.4.3", "https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip",
     "4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab", "cmake-4.4.3-windows-x86_64.zip", Packaging::Archive, "cmake",
     "cmake/cmake-4.4.3-windows-x86_64/bin"},
    {"ninja", "Ninja 1.13.2", "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
     "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", "ninja-win.zip", Packaging::Archive, "ninja", "ninja"},
    {"g++", "MinGW-w64 GCC 15.2 (nuwen.net 20.0)", "https://nuwen.net/files/mingw/mingw-20.0.exe",
     "bce8bdaa095848561488d34fd80371a1f36eb0a590202eb534f32e026e5cd8ef", "mingw-20.0.exe", Packaging::SelfExtracting, "mingw", "mingw/MinGW/bin"},
};
constexpr const char* kPython = "python";
constexpr const char* kCompiler = "g++";
#elif defined(__linux__) && defined(__x86_64__)
constexpr Package kPackages[] = {
    {"nodtool", "nodtool 1.4.4", "https://github.com/encounter/nod/releases/download/v1.4.4/nodtool-linux-x86_64",
     "cf20828d1a437ca14ed7acfd5545e1a0e62c082f5a77240a399aa4f172119fc4", "nodtool-linux-x86_64", Packaging::Single, "nodtool", "nodtool"},
};
constexpr const char* kPython = "python3";
constexpr const char* kCompiler = "c++";
#elif defined(__linux__) && defined(__aarch64__)
constexpr Package kPackages[] = {
    {"nodtool", "nodtool 1.4.4", "https://github.com/encounter/nod/releases/download/v1.4.4/nodtool-linux-aarch64",
     "4180f9501dc0de2fb4841c865604fad91899798280f49015e7be8f89890ee37e", "nodtool-linux-aarch64", Packaging::Single, "nodtool", "nodtool"},
};
constexpr const char* kPython = "python3";
constexpr const char* kCompiler = "c++";
#else
constexpr Package kPackages[] = {{nullptr, nullptr, nullptr, nullptr, nullptr, Packaging::Single, nullptr, nullptr}};
constexpr const char* kPython = "python3";
constexpr const char* kCompiler = "c++";
#endif

struct Requirement {
    const char* program;
    const char* argument;
    const char* pattern;
    int major;
    int minor;
};

const Package* package_for(const std::string& program) {
    for (const Package& package : kPackages) {
        if (package.program && program == package.program) {
            return &package;
        }
    }
    return nullptr;
}

std::string system_program(const char* name) {
#ifdef _WIN32
    std::string root = environment_value("SystemRoot");
    return utf8_of(path_from(root.empty() ? "C:/Windows" : root) / "System32" / (std::string(name) + ".exe"));
#else
    return name;
#endif
}

}

Toolchain::Toolchain(std::filesystem::path folder, std::filesystem::path bundled) : folder_(std::move(folder)), bundled_(std::move(bundled)) {}

void Toolchain::add_to_path() const {
    std::vector<std::filesystem::path> folders;
    for (const std::filesystem::path& base : {bundled_, folder_}) {
        for (const Package& package : kPackages) {
            if (package.bin) {
                folders.push_back(base / path_from(package.bin));
            }
        }
    }
    prepend_path(folders);
}

std::string Toolchain::python() const {
    return kPython;
}

std::vector<ToolStatus> Toolchain::inspect() const {
    const Requirement requirements[] = {
        {"nodtool", "--version", R"(nodtool)", 0, 0},
        {kPython, "--version", R"(Python (\d+)\.(\d+))", 3, 11},
        {"cmake", "--version", R"(cmake version (\d+)\.(\d+))", 3, 20},
        {"ninja", "--version", R"((\d+)\.(\d+))", 1, 10},
        {kCompiler, "-dumpfullversion", R"((\d+)\.(\d+))", 13, 0},
    };
    std::vector<ToolStatus> tools;
    for (const Requirement& requirement : requirements) {
        ToolStatus status;
        status.program = requirement.program;
        std::string path = find_program(status.program);
        std::error_code outside;
        std::filesystem::path relative = std::filesystem::relative(path_from(path), bundled_, outside);
        status.bundled = !path.empty() && !outside && !relative.empty() && relative.native()[0] != '.';
        std::smatch match;
        std::string text = path.empty() ? std::string() : capture(path, {requirement.argument});
        if (!text.empty() && std::regex_search(text, match, std::regex(requirement.pattern))) {
            status.version = match.str(0);
            int major = match.size() > 2 ? std::atoi(match.str(1).c_str()) : 0;
            int minor = match.size() > 2 ? std::atoi(match.str(2).c_str()) : 0;
            status.usable = major > requirement.major || (major == requirement.major && minor >= requirement.minor);
#ifdef _WIN32
            if (status.usable && std::strcmp(requirement.program, kCompiler) == 0) {
                status.usable = capture(path, {"-dumpmachine"}).find("mingw32") != std::string::npos;
            }
#endif
        }
        tools.push_back(status);
    }
    return tools;
}

std::vector<Step> Toolchain::preparation(const std::vector<ToolStatus>& tools, bool offline) const {
    const Texts& t = texts();
    std::vector<Step> steps;
    std::filesystem::path downloads = folder_ / "downloads";
    for (const ToolStatus& tool : tools) {
        if (tool.usable) {
            continue;
        }
        const Package* package = package_for(tool.program);
        if (!package || offline) {
            std::string program = tool.program;
            Step step;
            step.title = format(t.step_tool, program);
            step.action = [program, offline](std::string& message) {
                message = format(offline ? texts().tool_offline : texts().tool_unavailable, program);
                return false;
            };
            steps.push_back(step);
            continue;
        }
        std::filesystem::path archive = downloads / package->file;
        std::filesystem::path destination = folder_ / package->folder;
        std::string name = package->name;
        std::string expected = package->sha256;
        Step download;
        download.title = format(t.step_download, name);
        download.program = system_program("curl");
        download.arguments = {"-L", "--fail", "--silent", "--show-error", "--create-dirs", "-o", utf8_of(archive), package->url};
        download.weight = 2.0f;
        steps.push_back(download);
        Step verify;
        verify.title = format(t.step_verify, name);
        verify.action = [archive, expected, destination](std::string& message) {
            std::error_code error;
            if (sha256_of(archive) != expected) {
                std::filesystem::remove(archive, error);
                message = format(texts().hash_mismatch, utf8_of(archive.filename()));
                return false;
            }
            std::filesystem::remove_all(destination, error);
            std::filesystem::create_directories(destination, error);
            return true;
        };
        steps.push_back(verify);
        Step unpack;
        unpack.title = format(t.step_install, name);
        switch (package->packaging) {
        case Packaging::Archive:
            unpack.program = system_program("tar");
            unpack.arguments = {"-xf", utf8_of(archive), "-C", utf8_of(destination)};
            break;
        case Packaging::SelfExtracting:
            unpack.program = utf8_of(archive);
            unpack.arguments = {"-y", "-o" + utf8_of(destination.lexically_normal().make_preferred())};
            break;
        case Packaging::Single:
            unpack.action = [archive, destination, program = tool.program](std::string&) {
                std::error_code error;
                std::filesystem::path target = destination / program;
#ifdef _WIN32
                target += ".exe";
                return std::filesystem::copy_file(archive, target, std::filesystem::copy_options::overwrite_existing, error);
#else
                if (!std::filesystem::copy_file(archive, target, std::filesystem::copy_options::overwrite_existing, error)) {
                    return false;
                }
                std::filesystem::permissions(target, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                             std::filesystem::perm_options::add, error);
                return !error;
#endif
            };
            break;
        }
        steps.push_back(unpack);
        Step finish;
        finish.title = format(t.step_finish, name);
        finish.action = [archive, destination](std::string&) {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(destination, error)) {
                if (entry.path().extension() == "._pth") {
                    std::filesystem::remove(entry.path(), error);
                }
            }
            std::filesystem::remove(archive, error);
            return true;
        };
        steps.push_back(finish);
    }
    return steps;
}
