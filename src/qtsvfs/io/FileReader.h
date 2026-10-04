#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace qtsvfs {

// 只读定位文件读取器：QtsVFS 包体内最大 1.1GB 且条目按文件内偏移寻址，
// 需要 pread 语义而不是顺序读。调用方始终以只读方式打开。
class FileReader {
public:
    FileReader() = default;
    ~FileReader() { close(); }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    FileReader(FileReader&& o) noexcept { swap(o); }
    FileReader& operator=(FileReader&& o) noexcept {
        if (this != &o) { close(); swap(o); }
        return *this;
    }

    bool open(const std::filesystem::path& p, std::string& err);
    void close();

    std::uint64_t size() const noexcept { return size_; }
    const std::filesystem::path& path() const noexcept { return path_; }
    bool isOpen() const noexcept { return fp_ != nullptr; }

    bool read(std::uint64_t offset, void* dst, std::size_t len, std::string& err);
    std::vector<std::uint8_t> read(std::uint64_t offset, std::size_t len, std::string& err);

private:
    void swap(FileReader& o) noexcept {
        std::swap(fp_, o.fp_);
        std::swap(path_, o.path_);
        std::swap(size_, o.size_);
    }

    std::FILE* fp_ = nullptr;
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
};

}  // namespace qtsvfs
