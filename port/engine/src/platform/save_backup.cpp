#include "wp/save_backup.h"
#include "wp/title.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <tuple>

namespace wp::saves {

namespace {

namespace fs = std::filesystem;

constexpr char kStampPattern[] = "0000-00-00_00-00-00";
constexpr size_t kStampLength = sizeof(kStampPattern) - 1;
constexpr size_t kMaximumSuffixDigits = 6;
constexpr size_t kChunkSize = 64 * 1024;
std::string title_type() {
    char type[16];
    std::snprintf(type, sizeof type, "%08x", static_cast<unsigned int>(title::title_high()));
    return type;
}
constexpr const char* kPartial = ".partial";

struct Entry {
    fs::path relative;
    bool directory;
    uintmax_t size;

    bool operator==(const Entry& other) const {
        return relative == other.relative && directory == other.directory && size == other.size;
    }
};

std::string ascii(const fs::path& path) {
    std::string value;
    for (auto character : path.native()) {
        value.push_back(static_cast<unsigned long>(character) < 0x80 ? static_cast<char>(character) : '?');
    }
    return value;
}

std::string text(const fs::path& path) {
    try {
        return path.string();
    } catch (const std::exception&) {
        return ascii(path);
    }
}

bool stamp_name(const std::string& name, unsigned long& suffix) {
    if (name.size() < kStampLength) {
        return false;
    }
    for (size_t i = 0; i < kStampLength; i++) {
        bool digit = std::isdigit(static_cast<unsigned char>(name[i])) != 0;
        if (kStampPattern[i] == '0' ? !digit : name[i] != kStampPattern[i]) {
            return false;
        }
    }
    suffix = 1;
    if (name.size() == kStampLength) {
        return true;
    }
    if (name[kStampLength] != '_' || name.size() == kStampLength + 1 || name.size() > kStampLength + 1 + kMaximumSuffixDigits) {
        return false;
    }
    suffix = 0;
    for (size_t i = kStampLength + 1; i < name.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            return false;
        }
        suffix = suffix * 10 + static_cast<unsigned long>(name[i] - '0');
    }
    return true;
}

std::string stamp(std::time_t now) {
    char value[32] = {};
    std::tm* local = std::localtime(&now);
    if (!local || std::strftime(value, sizeof value, "%Y-%m-%d_%H-%M-%S", local) == 0) {
        return "0000-00-00_00-00-00";
    }
    return value;
}

bool entries(const fs::path& root, std::vector<Entry>& out) {
    std::error_code error;
    if (!fs::is_directory(root, error)) {
        return false;
    }
    fs::recursive_directory_iterator it(root, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        Entry entry{it->path().lexically_relative(root), it->is_directory(error), 0};
        if (error) {
            return false;
        }
        if (!entry.directory) {
            entry.size = it->file_size(error);
            if (error) {
                return false;
            }
        }
        out.push_back(entry);
    }
    if (error) {
        return false;
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.relative < b.relative; });
    return true;
}

bool files_equal(const fs::path& first, const fs::path& second) {
    std::ifstream a(first, std::ios::binary);
    std::ifstream b(second, std::ios::binary);
    if (!a || !b) {
        return false;
    }
    std::vector<char> left(kChunkSize);
    std::vector<char> right(kChunkSize);
    while (true) {
        a.read(left.data(), static_cast<std::streamsize>(left.size()));
        b.read(right.data(), static_cast<std::streamsize>(right.size()));
        std::streamsize count = a.gcount();
        if (count != b.gcount() || std::memcmp(left.data(), right.data(), static_cast<size_t>(count)) != 0) {
            return false;
        }
        if (count == 0 || a.eof() || b.eof()) {
            return a.eof() == b.eof() && !a.bad() && !b.bad();
        }
    }
}

bool copy_tree(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::create_directories(to.parent_path(), error);
    if (error) {
        return false;
    }
    fs::copy(from, to, fs::copy_options::recursive, error);
    return !error;
}

void remove_partials(const fs::path& backups_root) {
    std::error_code error;
    std::vector<fs::path> stale;
    for (const auto& entry : fs::directory_iterator(backups_root, error)) {
        std::string name = ascii(entry.path().filename());
        size_t length = std::strlen(kPartial);
        unsigned long suffix = 0;
        if (name.size() > length && name.compare(name.size() - length, length, kPartial) == 0 &&
            stamp_name(name.substr(0, name.size() - length), suffix)) {
            stale.push_back(entry.path());
        }
    }
    for (const fs::path& path : stale) {
        fs::remove_all(path, error);
    }
}

std::string unique_name(const fs::path& root, const std::string& base) {
    std::error_code error;
    std::string name = base;
    for (int number = 2; fs::exists(root / name, error) || fs::exists(root / (name + kPartial), error); number++) {
        name = base + "_" + std::to_string(number);
    }
    return name;
}

Outcome make_outcome(Result result, const std::string& message) {
    Outcome outcome;
    outcome.result = result;
    outcome.message = message;
    return outcome;
}

Outcome back_up_unguarded(const fs::path& nand_root, const fs::path& relative, const fs::path& backups_root, int keep, std::time_t now) {
    fs::path save = nand_root / relative;
    std::error_code error;
    if (!fs::is_directory(save, error)) {
        return make_outcome(Result::NoSave, "no save folder at " + text(save));
    }
    std::string problem;
    if (!validate(save, problem)) {
        return make_outcome(Result::Invalid, "the save in " + text(save) + " failed the check: " + problem);
    }
    std::vector<Backup> backups = list(backups_root);
    if (!backups.empty() && same_contents(save, backups.front().path / relative)) {
        rotate(backups_root, keep);
        Outcome outcome = make_outcome(Result::Unchanged, "the save matches the newest backup " + backups.front().name);
        outcome.backup = backups.front().path;
        return outcome;
    }
    fs::create_directories(backups_root, error);
    if (error) {
        return make_outcome(Result::Failed, "cannot create " + text(backups_root) + ": " + error.message());
    }
    remove_partials(backups_root);
    std::string name = unique_name(backups_root, stamp(now));
    fs::path partial = backups_root / (name + kPartial);
    fs::path target = backups_root / name;
    if (!copy_tree(save, partial / relative) || !same_contents(save, partial / relative)) {
        fs::remove_all(partial, error);
        return make_outcome(Result::Failed, "cannot copy the save to " + text(partial));
    }
    fs::rename(partial, target, error);
    if (error) {
        std::string message = "cannot rename " + text(partial) + ": " + error.message();
        fs::remove_all(partial, error);
        return make_outcome(Result::Failed, message);
    }
    rotate(backups_root, keep);
    Outcome outcome = make_outcome(Result::Created, "backed up the save to " + text(target));
    outcome.backup = target;
    return outcome;
}

Outcome restore_unguarded(const fs::path& nand_root, const fs::path& backups_root, const Backup& backup, int keep, std::time_t now) {
    fs::path relative = saved_relative(backup);
    if (relative.empty()) {
        return make_outcome(Result::Failed, "the backup " + backup.name + " holds no save");
    }
    fs::path source = backup.path / relative;
    std::string problem;
    if (!validate(source, problem)) {
        return make_outcome(Result::Invalid, "the backup " + backup.name + " failed the check: " + problem);
    }
    fs::path live = nand_root / relative;
    std::error_code error;
    bool present = fs::exists(live, error);
    if (present && same_contents(live, source)) {
        return make_outcome(Result::Unchanged, "the save already matches the backup " + backup.name);
    }
    fs::path kept;
    if (present) {
        Outcome safety = back_up_unguarded(nand_root, relative, backups_root, 0, now);
        if (safety.result == Result::Invalid) {
            kept = backups_root / unique_name(backups_root, "damaged-" + stamp(now)) / relative;
        } else if (safety.result != Result::Created && safety.result != Result::Unchanged) {
            return make_outcome(Result::Failed, "the current save could not be backed up first: " + safety.message);
        }
    }
    fs::path staging = live;
    staging += ".restoring";
    fs::remove_all(staging, error);
    if (!copy_tree(source, staging) || !same_contents(source, staging)) {
        fs::remove_all(staging, error);
        return make_outcome(Result::Failed, "cannot copy the backup " + backup.name + " into " + text(staging));
    }
    fs::path old = live;
    old += ".replaced";
    if (!kept.empty()) {
        old = kept;
    }
    if (present) {
        if (kept.empty()) {
            fs::remove_all(old, error);
        }
        fs::create_directories(old.parent_path(), error);
        fs::rename(live, old, error);
        if (error) {
            std::string message = "cannot move the current save aside: " + error.message();
            fs::remove_all(staging, error);
            return make_outcome(Result::Failed, message);
        }
    }
    fs::rename(staging, live, error);
    if (error) {
        std::string message = "cannot put the backup in place: " + error.message();
        if (present) {
            fs::rename(old, live, error);
        }
        fs::remove_all(staging, error);
        return make_outcome(Result::Failed, message);
    }
    if (present && kept.empty()) {
        fs::remove_all(old, error);
    }
    rotate(backups_root, keep);
    std::string message = "restored the backup " + backup.name;
    if (!kept.empty()) {
        message += "; the damaged save was moved to " + text(kept);
    }
    return make_outcome(Result::Created, message);
}

}

fs::path save_relative(uint32_t title_low) {
    char name[16];
    std::snprintf(name, sizeof name, "%08x", static_cast<unsigned int>(title_low));
    return fs::path("title") / title_type() / name / "data";
}

fs::path backups_beside(const fs::path& nand_root) {
    fs::path root = nand_root;
    if (!root.has_filename()) {
        root = root.parent_path();
    }
    return root.parent_path() / "backups";
}

fs::path saved_relative(const Backup& backup) {
    std::error_code error;
    fs::path titles = backup.path / "title" / title_type();
    std::vector<fs::path> names;
    try {
        for (const auto& entry : fs::directory_iterator(titles, error)) {
            if (fs::is_directory(entry.path() / "data", error)) {
                names.push_back(entry.path().filename());
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    if (names.empty()) {
        return {};
    }
    std::sort(names.begin(), names.end());
    return fs::path("title") / title_type() / names.front() / "data";
}

std::vector<Backup> list(const fs::path& backups_root) {
    std::vector<std::tuple<std::string, unsigned long, Backup>> found;
    try {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(backups_root, error)) {
            std::string name = ascii(entry.path().filename());
            unsigned long suffix = 0;
            if (stamp_name(name, suffix) && entry.is_directory(error)) {
                found.emplace_back(name.substr(0, kStampLength), suffix, Backup{name, entry.path()});
            }
        }
    } catch (const std::exception&) {
        found.clear();
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return std::tie(std::get<0>(a), std::get<1>(a)) > std::tie(std::get<0>(b), std::get<1>(b));
    });
    std::vector<Backup> backups;
    for (auto& item : found) {
        backups.push_back(std::move(std::get<2>(item)));
    }
    return backups;
}

bool validate(const fs::path& save, std::string& problem) {
    std::vector<Entry> files;
    if (!entries(save, files)) {
        problem = "cannot list " + text(save);
        return false;
    }
    std::vector<char> buffer(kChunkSize);
    for (const Entry& entry : files) {
        if (entry.directory) {
            continue;
        }
        std::ifstream file(save / entry.relative, std::ios::binary);
        if (!file) {
            problem = text(entry.relative) + " cannot be opened";
            return false;
        }
        uintmax_t count = 0;
        while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0) {
            count += static_cast<uintmax_t>(file.gcount());
            if (file.eof()) {
                break;
            }
        }
        if (file.bad() || count != entry.size) {
            problem = text(entry.relative) + " gave " + std::to_string(count) + " of its " + std::to_string(entry.size) + " bytes";
            return false;
        }
    }
    return true;
}

bool same_contents(const fs::path& first, const fs::path& second) {
    std::vector<Entry> a;
    std::vector<Entry> b;
    if (!entries(first, a) || !entries(second, b) || !(a == b)) {
        return false;
    }
    for (const Entry& entry : a) {
        if (!entry.directory && !files_equal(first / entry.relative, second / entry.relative)) {
            return false;
        }
    }
    return true;
}

Outcome back_up(const fs::path& nand_root, const fs::path& relative, const fs::path& backups_root, int keep, std::time_t now) {
    try {
        return back_up_unguarded(nand_root, relative, backups_root, keep, now);
    } catch (const std::exception& exception) {
        return make_outcome(Result::Failed, exception.what());
    }
}

Outcome restore(const fs::path& nand_root, const fs::path& backups_root, const Backup& backup, int keep, std::time_t now) {
    try {
        return restore_unguarded(nand_root, backups_root, backup, keep, now);
    } catch (const std::exception& exception) {
        return make_outcome(Result::Failed, exception.what());
    }
}

void rotate(const fs::path& backups_root, int keep) {
    if (keep <= 0) {
        return;
    }
    std::vector<Backup> backups = list(backups_root);
    std::error_code error;
    for (size_t i = static_cast<size_t>(keep); i < backups.size(); i++) {
        fs::remove_all(backups[i].path, error);
    }
}

}
