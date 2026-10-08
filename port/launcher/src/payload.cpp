#include "payload.h"

#include <fstream>
#include <iterator>
#include <set>

#include "embedded.h"
#include "sha256.h"
#include "subprocess.h"
#include "texts.h"

namespace {

constexpr const char* kRevisionFile = ".launcher-revision";
constexpr const char* kManifestFile = ".launcher-files";
constexpr const char* kStagingFolder = ".launcher-staging";
constexpr const char* kArchiveFile = ".launcher-payload.tar.gz";

constexpr const char* kRebuildTriggers[] = {
    "CMakeLists.txt",
    "games/wiiparty/CMakeLists.txt",
    "engine/include/wp/cpu.h",
    "engine/include/wp/modules.h",
    "engine/include/wp/threads.h",
    "engine/include/wp/hle.h",
    "engine/include/wp/function_table.h",
    "recompiler/recomp.py",
    "recompiler/recomp_rel.py",
    "recompiler/ppc/",
};

std::string tar_program() {
#ifdef _WIN32
    std::string system = environment_value("SystemRoot");
    return utf8_of(path_from(system.empty() ? "C:/Windows" : system) / "System32" / "tar.exe");
#else
    return find_program("tar");
#endif
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<std::string> staged_files(const std::filesystem::path& staging) {
    std::vector<std::string> files;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(staging, error); !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (it->is_regular_file(error)) {
            std::string relative = std::filesystem::relative(it->path(), staging, error).generic_u8string();
            if (relative != kRevisionFile) {
                files.push_back(relative);
            }
        }
    }
    return files;
}

bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    if (!std::filesystem::exists(b, error) || std::filesystem::file_size(a, error) != std::filesystem::file_size(b, error)) {
        return false;
    }
    return read_file(a) == read_file(b);
}

}

const std::string& payload_revision() {
    static const std::string revision = [] {
        std::string base = source_revision();
        const std::string dirty = "-dirty";
        if (base.size() < dirty.size() || base.compare(base.size() - dirty.size(), dirty.size(), dirty) != 0) {
            return base;
        }
        return base + "-" + sha256_of_bytes(blob("payload")).substr(0, 12);
    }();
    return revision;
}

bool payload_available() {
    return !blob("payload").empty();
}

std::string stored_revision(const std::filesystem::path& root) {
    std::ifstream file(root / kRevisionFile, std::ios::binary);
    std::string revision;
    std::getline(file, revision);
    return revision;
}

bool stage_payload(const std::filesystem::path& root, std::string& message) {
    std::filesystem::path staging = root / kStagingFolder;
    if (stored_revision(staging) == payload_revision()) {
        return true;
    }
    std::error_code error;
    std::filesystem::remove_all(staging, error);
    std::filesystem::create_directories(staging, error);
    std::string_view data = blob("payload");
    {
        std::ofstream file(staging / kArchiveFile, std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) {
            message = format(texts().extract_failed, utf8_of(root));
            return false;
        }
    }
    Process tar;
    std::string failure;
    bool started = tar.start(tar_program(), {"-xzf", kArchiveFile}, staging, false, failure);
    char buffer[1024];
    while (started && tar.read(buffer, sizeof(buffer))) {
    }
    bool extracted = started && tar.wait() == 0;
    std::filesystem::remove(staging / kArchiveFile, error);
    if (!extracted) {
        message = format(texts().extract_failed, utf8_of(root)) + (failure.empty() ? "" : " " + failure);
        return false;
    }
    std::ofstream revision(staging / kRevisionFile, std::ios::binary | std::ios::trunc);
    revision << payload_revision() << '\n';
    return static_cast<bool>(revision);
}

PayloadChanges payload_changes(const std::filesystem::path& root) {
    PayloadChanges changes;
    std::filesystem::path staging = root / kStagingFolder;
    std::vector<std::string> files = staged_files(staging);
    std::set<std::string> present(files.begin(), files.end());
    for (const std::string& file : files) {
        if (!same_file(staging / path_from(file), root / path_from(file))) {
            changes.changed.push_back(file);
            for (const char* trigger : kRebuildTriggers) {
                std::string prefix = trigger;
                if (file == prefix || (prefix.back() == '/' && file.rfind(prefix, 0) == 0)) {
                    changes.full_rebuild = true;
                }
            }
        }
    }
    std::ifstream manifest(root / kManifestFile, std::ios::binary);
    for (std::string line; std::getline(manifest, line);) {
        if (!line.empty() && !present.count(line)) {
            changes.removed.push_back(line);
        }
    }
    return changes;
}

bool extract_payload(const std::filesystem::path& root, std::string& message) {
    if (stored_revision(root) == payload_revision()) {
        return true;
    }
    if (!stage_payload(root, message)) {
        return false;
    }
    std::filesystem::path staging = root / kStagingFolder;
    PayloadChanges changes = payload_changes(root);
    std::error_code error;
    for (const std::string& file : changes.changed) {
        std::filesystem::path target = root / path_from(file);
        std::filesystem::create_directories(target.parent_path(), error);
        std::filesystem::copy_file(staging / path_from(file), target, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            message = format(texts().extract_failed, utf8_of(root)) + " " + error.message();
            return false;
        }
    }
    for (const std::string& file : changes.removed) {
        std::filesystem::remove(root / path_from(file), error);
    }
    {
        std::ofstream manifest(root / kManifestFile, std::ios::binary | std::ios::trunc);
        for (const std::string& file : staged_files(staging)) {
            manifest << file << '\n';
        }
    }
    std::filesystem::remove_all(staging, error);
    std::ofstream revision(root / kRevisionFile, std::ios::binary | std::ios::trunc);
    revision << payload_revision() << '\n';
    message = format(texts().extracted_files, utf8_of(root));
    return true;
}
