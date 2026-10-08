#include "updater.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

#include "sha256.h"
#include "subprocess.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <sys/stat.h>
#endif

namespace {

constexpr const char* kReleasesUrl = "https://api.github.com/repos/arelkair/wiiparty-recomp/releases?per_page=30";
#ifdef _WIN32
constexpr const char* kAssetName = "wiipartyrecomp-launcher.exe";
#else
constexpr const char* kAssetName = "wiipartyrecomp-launcher-x86_64.AppImage";
#endif

struct Json {
    enum class Kind { Null, Boolean, Number, String, Array, Object } kind = Kind::Null;
    std::string text;
    bool flag = false;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json& operator[](const std::string& key) const {
        static const Json empty;
        auto found = fields.find(key);
        return found == fields.end() ? empty : found->second;
    }
};

class Parser {
public:
    explicit Parser(const std::string& source) : source_(source) {}

    bool parse(Json& value) {
        skip();
        if (at_ >= source_.size()) {
            return false;
        }
        char c = source_[at_];
        if (c == '{') {
            value.kind = Json::Kind::Object;
            at_++;
            skip();
            if (peek('}')) {
                return true;
            }
            while (true) {
                skip();
                Json key;
                if (!string(key.text)) {
                    return false;
                }
                skip();
                if (!peek(':')) {
                    return false;
                }
                Json item;
                if (!parse(item)) {
                    return false;
                }
                value.fields[key.text] = std::move(item);
                skip();
                if (peek('}')) {
                    return true;
                }
                if (!peek(',')) {
                    return false;
                }
            }
        }
        if (c == '[') {
            value.kind = Json::Kind::Array;
            at_++;
            skip();
            if (peek(']')) {
                return true;
            }
            while (true) {
                Json item;
                if (!parse(item)) {
                    return false;
                }
                value.items.push_back(std::move(item));
                skip();
                if (peek(']')) {
                    return true;
                }
                if (!peek(',')) {
                    return false;
                }
            }
        }
        if (c == '"') {
            value.kind = Json::Kind::String;
            return string(value.text);
        }
        if (source_.compare(at_, 4, "true") == 0 || source_.compare(at_, 5, "false") == 0) {
            value.kind = Json::Kind::Boolean;
            value.flag = source_[at_] == 't';
            at_ += value.flag ? 4 : 5;
            return true;
        }
        if (source_.compare(at_, 4, "null") == 0) {
            at_ += 4;
            return true;
        }
        size_t start = at_;
        while (at_ < source_.size() && (std::isdigit(static_cast<unsigned char>(source_[at_])) || std::strchr("+-.eE", source_[at_]))) {
            at_++;
        }
        value.kind = Json::Kind::Number;
        value.text = source_.substr(start, at_ - start);
        return at_ > start;
    }

private:
    void skip() {
        while (at_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[at_]))) {
            at_++;
        }
    }

    bool peek(char c) {
        if (at_ < source_.size() && source_[at_] == c) {
            at_++;
            return true;
        }
        return false;
    }

    bool string(std::string& out) {
        if (!peek('"')) {
            return false;
        }
        while (at_ < source_.size()) {
            char c = source_[at_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= source_.size()) {
                return false;
            }
            char escaped = source_[at_++];
            if (escaped == 'u') {
                if (at_ + 4 > source_.size()) {
                    return false;
                }
                unsigned code = static_cast<unsigned>(std::strtoul(source_.substr(at_, 4).c_str(), nullptr, 16));
                at_ += 4;
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
            } else {
                const char* from = "\"\\/bfnrt";
                const char* to = "\"\\/\b\f\n\r\t";
                const char* found = std::strchr(from, escaped);
                out += found ? to[found - from] : escaped;
            }
        }
        return false;
    }

    const std::string& source_;
    size_t at_ = 0;
};

struct Version {
    int numbers[3] = {0, 0, 0};
    std::vector<std::string> pre;
};

Version parse_version(std::string text) {
    Version version;
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) {
        text.erase(0, 1);
    }
    size_t dash = text.find('-');
    std::string core = text.substr(0, dash);
    size_t start = 0;
    for (int i = 0; i < 3; i++) {
        size_t dot = core.find('.', start);
        version.numbers[i] = std::atoi(core.substr(start, dot - start).c_str());
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    if (dash != std::string::npos) {
        std::string pre = text.substr(dash + 1);
        size_t from = 0;
        while (from <= pre.size()) {
            size_t dot = pre.find('.', from);
            version.pre.push_back(pre.substr(from, dot - from));
            if (dot == std::string::npos) {
                break;
            }
            from = dot + 1;
        }
    }
    return version;
}

bool numeric(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

int compare(const Version& a, const Version& b) {
    for (int i = 0; i < 3; i++) {
        if (a.numbers[i] != b.numbers[i]) {
            return a.numbers[i] < b.numbers[i] ? -1 : 1;
        }
    }
    if (a.pre.empty() || b.pre.empty()) {
        return a.pre.empty() == b.pre.empty() ? 0 : (a.pre.empty() ? 1 : -1);
    }
    for (size_t i = 0; i < std::min(a.pre.size(), b.pre.size()); i++) {
        const std::string& x = a.pre[i];
        const std::string& y = b.pre[i];
        if (x == y) {
            continue;
        }
        if (numeric(x) && numeric(y)) {
            return std::atoi(x.c_str()) < std::atoi(y.c_str()) ? -1 : 1;
        }
        if (numeric(x) != numeric(y)) {
            return numeric(x) ? -1 : 1;
        }
        return x < y ? -1 : 1;
    }
    return a.pre.size() == b.pre.size() ? 0 : (a.pre.size() < b.pre.size() ? -1 : 1);
}

#ifdef _WIN32

std::wstring wide(const std::string& text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(size, 1)) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    return result;
}

struct Handle {
    HINTERNET value = nullptr;
    ~Handle() {
        if (value) {
            WinHttpCloseHandle(value);
        }
    }
};

template <typename Sink>
bool fetch(const std::string& url, const wchar_t* accept, Sink sink, std::string& error) {
    std::wstring address = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256];
    wchar_t path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "bad address";
        return false;
    }
    std::wstring object = std::wstring(path, parts.dwUrlPathLength) + std::wstring(extra, parts.dwExtraInfoLength);
    Handle session{WinHttpOpen(L"wiipartyrecomp-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) {
        error = "network unavailable";
        return false;
    }
    WinHttpSetTimeouts(session.value, 10000, 10000, 20000, 60000);
    Handle connection{WinHttpConnect(session.value, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0)};
    Handle request{connection.value ? WinHttpOpenRequest(connection.value, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         WINHTTP_FLAG_SECURE)
                                    : nullptr};
    if (!request.value) {
        error = "connection failed";
        return false;
    }
    std::wstring headers = std::wstring(L"Accept: ") + accept;
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        error = "request failed";
        return false;
    }
    DWORD status = 0;
    DWORD length = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    char buffer[65536];
    DWORD read = 0;
    while (WinHttpReadData(request.value, buffer, sizeof(buffer), &read) && read > 0) {
        if (!sink(buffer, read)) {
            error = "write failed";
            return false;
        }
    }
    return true;
}

bool fetch_text(const std::string& url, std::string& body, std::string& error) {
    return fetch(url, L"application/vnd.github+json", [&](const char* data, size_t size) {
        body.append(data, size);
        return true;
    }, error);
}

#else

bool curl(const std::vector<std::string>& arguments, std::string& output, std::string& error) {
    std::string program = find_program("curl");
    if (environment_value("APPIMAGE").empty() || program.empty()) {
        error = "not supported";
        return false;
    }
    std::vector<std::string> all = {"-L", "--fail", "--silent", "--show-error", "--connect-timeout", "10"};
    all.insert(all.end(), arguments.begin(), arguments.end());
    Process process;
    if (!process.start(program, all, {}, false, error)) {
        return false;
    }
    char buffer[65536];
    while (size_t count = process.read(buffer, sizeof(buffer))) {
        output.append(buffer, count);
    }
    if (process.wait() != 0) {
        error = output.empty() ? "request failed" : output.substr(0, output.find('\n'));
        return false;
    }
    return true;
}

bool fetch_text(const std::string& url, std::string& body, std::string& error) {
    return curl({"-H", "Accept: application/vnd.github+json", url}, body, error);
}

#endif

}

bool newer_version(const std::string& candidate, const std::string& current) {
    return compare(parse_version(candidate), parse_version(current)) > 0;
}

bool latest_release(Release& release, Release& current, const std::string& current_version, std::string& error) {
    std::string body;
    if (!fetch_text(kReleasesUrl, body, error)) {
        return false;
    }
    Json list;
    if (!Parser(body).parse(list) || list.kind != Json::Kind::Array) {
        error = "unexpected reply";
        return false;
    }
    bool found = false;
    for (const Json& entry : list.items) {
        if (entry["draft"].flag) {
            continue;
        }
        std::string tag = entry["tag_name"].text;
        std::string version = tag.rfind('v', 0) == 0 ? tag.substr(1) : tag;
        if (version == current_version) {
            std::string body = entry["body"].text;
            std::string first = body.substr(0, body.find_first_of("\r\n"));
            size_t dash = first.find("** - ");
            current.version = version;
            current.page = entry["html_url"].text;
            current.summary = dash == std::string::npos ? first : first.substr(dash + 5);
        }
        if (found && !newer_version(tag, release.version)) {
            continue;
        }
        for (const Json& asset : entry["assets"].items) {
            std::string digest = asset["digest"].text;
            if (asset["name"].text == kAssetName && digest.rfind("sha256:", 0) == 0) {
                release.version = tag.rfind('v', 0) == 0 ? tag.substr(1) : tag;
                release.url = asset["browser_download_url"].text;
                release.sha256 = digest.substr(7);
                release.page = entry["html_url"].text;
                std::string body = entry["body"].text;
                std::string first = body.substr(0, body.find_first_of("\r\n"));
                size_t dash = first.find("** - ");
                release.summary = dash == std::string::npos ? first : first.substr(dash + 5);
                found = true;
            }
        }
    }
    if (!found) {
        error = "no release";
    }
    return found;
}

bool download(const std::string& url, const std::filesystem::path& target, std::string& error) {
#ifdef _WIN32
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "write failed";
        return false;
    }
    bool ok = fetch(url, L"application/octet-stream", [&](const char* data, size_t size) {
        file.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(file);
    }, error);
    file.close();
    return ok && static_cast<bool>(file);
#else
    std::string output;
    if (!curl({"-o", utf8_of(target), url}, output, error)) {
        return false;
    }
    return chmod(target.c_str(), 0755) == 0;
#endif
}

std::filesystem::path launcher_path() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4];
    DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(std::wstring(buffer, size));
#else
    std::string image = environment_value("APPIMAGE");
    if (!image.empty()) {
        return path_from(image);
    }
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}

bool replace_launcher(const std::filesystem::path& downloaded, std::string& error) {
    std::filesystem::path current = launcher_path();
    std::filesystem::path old = current;
    old += ".old";
    std::error_code code;
    std::filesystem::remove(old, code);
    std::filesystem::rename(current, old, code);
    if (code) {
        error = code.message();
        return false;
    }
    std::filesystem::rename(downloaded, current, code);
    if (code) {
        std::error_code ignored;
        std::filesystem::rename(old, current, ignored);
        error = code.message();
        return false;
    }
    std::error_code ignored;
    if (!start_detached(utf8_of(current), std::filesystem::current_path(ignored))) {
        error = "restart failed";
        return false;
    }
    return true;
}

void remove_old_launcher() {
    std::filesystem::path old = launcher_path();
    old += ".old";
    std::error_code error;
    std::filesystem::remove(old, error);
}
