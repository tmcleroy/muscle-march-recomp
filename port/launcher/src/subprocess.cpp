#include "subprocess.h"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

#ifdef _WIN32
constexpr char kListSeparator = ';';

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void quote(std::wstring& line, const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line += L'"';
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            slashes++;
            continue;
        }
        if (c == L'"') {
            line.append(slashes * 2 + 1, L'\\');
        } else {
            line.append(slashes, L'\\');
        }
        slashes = 0;
        line += c;
    }
    line.append(slashes * 2, L'\\');
    line += L'"';
}

std::wstring command_line(const std::string& program, const std::vector<std::string>& arguments) {
    std::wstring line;
    quote(line, widen(program));
    for (const std::string& argument : arguments) {
        line += L' ';
        quote(line, widen(argument));
    }
    return line;
}

std::string system_message(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = text ? narrow(text) : "error " + std::to_string(code);
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
        message.pop_back();
    }
    return message;
}
#else
constexpr char kListSeparator = ':';
#endif

bool executable(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return false;
    }
#ifdef _WIN32
    return true;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

}

std::filesystem::path path_from(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(widen(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

std::string utf8_of(const std::filesystem::path& path) {
#ifdef _WIN32
    return narrow(path.wstring());
#else
    return path.string();
#endif
}

std::string environment_value(const char* name) {
#ifdef _WIN32
    const wchar_t* value = _wgetenv(widen(name).c_str());
    return value ? narrow(value) : std::string();
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

std::string find_program(const std::string& name) {
    std::filesystem::path direct = path_from(name);
    if (direct.has_parent_path()) {
        return executable(direct) ? name : std::string();
    }
    std::string list = environment_value("PATH");
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(kListSeparator, start);
        std::string folder = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? list.size() + 1 : end + 1;
        if (folder.empty() || folder.find("WindowsApps") != std::string::npos) {
            continue;
        }
        std::filesystem::path candidate = path_from(folder) / direct;
#ifdef _WIN32
        if (!candidate.has_extension()) {
            candidate += ".exe";
        }
#endif
        if (executable(candidate)) {
            return utf8_of(candidate);
        }
    }
    return {};
}

void prepend_path(const std::vector<std::filesystem::path>& folders) {
    std::string value;
    for (const std::filesystem::path& folder : folders) {
        value += utf8_of(folder.lexically_normal().make_preferred()) + kListSeparator;
    }
    value += environment_value("PATH");
#ifdef _WIN32
    _wputenv_s(L"PATH", widen(value).c_str());
#else
    setenv("PATH", value.c_str(), 1);
#endif
}

#ifdef _WIN32

Process::~Process() {
    if (output_) {
        CloseHandle(output_);
    }
    if (process_) {
        CloseHandle(process_);
    }
    if (job_) {
        CloseHandle(job_);
    }
}

bool Process::start(const std::string& program, const std::vector<std::string>& arguments, const std::filesystem::path& folder, bool low_priority,
                    std::string& error) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
        error = system_message(GetLastError());
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    std::wstring line = command_line(program, arguments);
    std::wstring directory = folder.wstring();
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | (low_priority ? BELOW_NORMAL_PRIORITY_CLASS : 0);
    PROCESS_INFORMATION info{};
    BOOL created = CreateProcessW(widen(program).c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr, directory.empty() ? nullptr : directory.c_str(),
                                  &startup, &info);
    DWORD code = GetLastError();
    CloseHandle(write_end);
    CloseHandle(input);
    if (!created) {
        CloseHandle(read_end);
        error = system_message(code);
        return false;
    }
    AssignProcessToJobObject(job_, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    output_ = read_end;
    return true;
}

size_t Process::read(char* buffer, size_t size) {
    DWORD count = 0;
    if (!output_ || !ReadFile(output_, buffer, static_cast<DWORD>(size), &count, nullptr)) {
        return 0;
    }
    return count;
}

int Process::wait() {
    if (!process_) {
        return -1;
    }
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process_, &code);
    return static_cast<int>(code);
}

void Process::kill() {
    if (job_) {
        TerminateJobObject(job_, 1);
    }
}

bool start_detached(const std::string& program, const std::filesystem::path& folder) {
    std::wstring line = command_line(program, {});
    std::wstring directory = folder.wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(widen(program).c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup, &info)) {
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

#else

Process::~Process() {
    if (output_ >= 0) {
        close(output_);
    }
}

bool Process::start(const std::string& program, const std::vector<std::string>& arguments, const std::filesystem::path& folder, bool low_priority,
                    std::string& error) {
    int ends[2];
    if (pipe(ends) != 0) {
        error = std::strerror(errno);
        return false;
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        error = std::strerror(errno);
        close(ends[0]);
        close(ends[1]);
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int input = open("/dev/null", O_RDONLY);
        dup2(input, 0);
        dup2(ends[1], 1);
        dup2(ends[1], 2);
        close(ends[0]);
        if (!folder.empty() && chdir(folder.c_str()) != 0) {
            _exit(127);
        }
        if (low_priority) {
            setpriority(PRIO_PROCESS, 0, 5);
        }
        execve(program.c_str(), argv.data(), environ);
        _exit(127);
    }
    close(ends[1]);
    pid_ = pid;
    output_ = ends[0];
    return true;
}

size_t Process::read(char* buffer, size_t size) {
    while (output_ >= 0) {
        ssize_t count = ::read(output_, buffer, size);
        if (count >= 0) {
            return static_cast<size_t>(count);
        }
        if (errno != EINTR) {
            break;
        }
    }
    return 0;
}

int Process::wait() {
    int status = 0;
    while (pid_ > 0 && waitpid(pid_, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void Process::kill() {
    if (pid_ > 0) {
        ::kill(-pid_, SIGKILL);
    }
}

bool start_detached(const std::string& program, const std::filesystem::path& folder) {
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        setsid();
        if (chdir(folder.c_str()) != 0) {
            _exit(127);
        }
        char* argv[] = {const_cast<char*>(program.c_str()), nullptr};
        execve(program.c_str(), argv, environ);
        _exit(127);
    }
    return true;
}

#endif

std::string capture(const std::string& program, const std::vector<std::string>& arguments) {
    Process process;
    std::string error;
    if (!process.start(program, arguments, {}, false, error)) {
        return {};
    }
    std::string text;
    char buffer[4096];
    while (size_t count = process.read(buffer, sizeof(buffer))) {
        text.append(buffer, count);
    }
    if (process.wait() != 0) {
        return {};
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}
