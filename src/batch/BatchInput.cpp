#include "BatchInput.hpp"
#include "PathUtf8.hpp"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace vr {

namespace {

bool HasExtension(const std::string& name, const char* ext) {
    size_t extLen = std::strlen(ext);
    if (name.size() < extLen) return false;
    std::string tail = name.substr(name.size() - extLen);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return std::tolower(c); });
    return tail == ext;
}

std::string MakeTempDirName() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "voiceripper_%016llx", static_cast<unsigned long long>(gen()));
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / buf;
    return tmp.string();
}

void CollectDemFilesRecursive(const std::filesystem::path& dir, std::vector<BatchInputItem>& out) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        if (HasExtension(name, ".dem")) {
            BatchInputItem item;
            item.path = it->path().string();
            item.displayName = name;
            out.push_back(std::move(item));
        }
    }
}

// Reads the whole file into memory via our UTF-8-safe path handling (zip
// archive paths may themselves contain non-ASCII characters), then hands
// the in-memory buffer to miniz -- avoids relying on miniz's own
// fopen()-based file I/O, which would mangle non-ASCII archive paths on
// Windows the same way raw std::ofstream(string) does elsewhere in this
// codebase.
bool ReadWholeFile(const std::string& utf8Path, std::vector<uint8_t>& out) {
    std::ifstream in(PathFromUtf8(utf8Path), std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool ExtractZip(const std::string& zipPathUtf8, BatchInputResult& result) {
    std::vector<uint8_t> zipBytes;
    if (!ReadWholeFile(zipPathUtf8, zipBytes)) {
        result.error = "cannot open zip file: " + zipPathUtf8;
        return false;
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) {
        result.error = "not a valid zip archive: " + zipPathUtf8;
        return false;
    }

    std::string tempDir = MakeTempDirName();
    std::error_code ec;
    std::filesystem::create_directories(PathFromUtf8(tempDir), ec);

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    bool anyExtracted = false;

    for (mz_uint i = 0; i < numFiles; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        std::string entryName = stat.m_filename;
        // Zip entry names use '/' regardless of platform; just need the
        // base filename for our extension check and display name.
        size_t slash = entryName.find_last_of('/');
        std::string baseName = (slash == std::string::npos) ? entryName : entryName.substr(slash + 1);
        if (!HasExtension(baseName, ".dem")) continue;

        size_t extractedSize = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &extractedSize, 0);
        if (!data) continue;

        // Flatten into the temp dir with a numeric prefix to avoid
        // collisions between same-named entries in different zip
        // subfolders, while keeping the original name visible.
        std::string flatName = std::to_string(i) + "_" + baseName;
        std::string destPath = tempDir + "/" + flatName;

        std::ofstream out(PathFromUtf8(destPath), std::ios::binary);
        if (out.is_open()) {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(extractedSize));
            out.close();

            BatchInputItem item;
            item.path = destPath;
            item.displayName = baseName;
            item.fromZip = true;
            result.items.push_back(std::move(item));
            anyExtracted = true;
        }

        mz_free(data);
    }

    mz_zip_reader_end(&zip);

    if (anyExtracted) {
        result.tempDirsToCleanup.push_back(tempDir);
    } else {
        std::filesystem::remove_all(PathFromUtf8(tempDir), ec);
    }
    return true;
}

} // namespace

BatchInputResult ResolveBatchInput(const std::string& inputPathUtf8) {
    BatchInputResult result;

    std::filesystem::path p = PathFromUtf8(inputPathUtf8);
    std::error_code ec;

    if (!std::filesystem::exists(p, ec)) {
        result.error = "path does not exist: " + inputPathUtf8;
        return result;
    }

    if (std::filesystem::is_directory(p, ec)) {
        CollectDemFilesRecursive(p, result.items);
        return result;
    }

    std::string fileName = p.filename().string();
    if (HasExtension(fileName, ".zip")) {
        ExtractZip(inputPathUtf8, result);
        return result;
    }

    if (HasExtension(fileName, ".dem")) {
        BatchInputItem item;
        item.path = inputPathUtf8;
        item.displayName = fileName;
        result.items.push_back(std::move(item));
        return result;
    }

    result.error = "not a .dem file, directory, or .zip archive: " + inputPathUtf8;
    return result;
}

} // namespace vr
