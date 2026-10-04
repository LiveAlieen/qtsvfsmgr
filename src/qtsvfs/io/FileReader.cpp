#include "qtsvfs/io/FileReader.h"

#include <filesystem>
#include <utility>

#ifdef _WIN32
#include <cstdio>
#endif

namespace qtsvfs {

bool FileReader::open(const std::filesystem::path& p, std::string& err) {
    close();
#ifdef _WIN32
    std::FILE* fp = nullptr;
    if (_wfopen_s(&fp, p.c_str(), L"rb") != 0 || fp == nullptr) {
        err = "无法以只读方式打开: " + p.string();
        return false;
    }
#else
    std::FILE* fp = std::fopen(p.c_str(), "rb");
    if (fp == nullptr) {
        err = "无法以只读方式打开: " + p.string();
        return false;
    }
#endif
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) {
        std::fclose(fp);
        err = "无法获取文件大小: " + p.string() + " (" + ec.message() + ")";
        return false;
    }
    fp_ = fp;
    path_ = p;
    size_ = sz;
    return true;
}

void FileReader::close() {
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    path_.clear();
    size_ = 0;
}

bool FileReader::read(std::uint64_t offset, void* dst, std::size_t len, std::string& err) {
    if (fp_ == nullptr) {
        err = "文件未打开";
        return false;
    }
    if (offset > size_ || len > size_ - offset) {
        err = "读取越界: offset=" + std::to_string(offset) + " len=" + std::to_string(len) +
              " size=" + std::to_string(size_);
        return false;
    }
    if (len == 0) {
        return true;
    }
#ifdef _WIN32
    if (_fseeki64(fp_, static_cast<std::int64_t>(offset), SEEK_SET) != 0) {
#else
    if (std::fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0) {
#endif
        err = "定位失败: offset=" + std::to_string(offset);
        return false;
    }
    if (std::fread(dst, 1, len, fp_) != len) {
        err = "读取失败: offset=" + std::to_string(offset);
        return false;
    }
    return true;
}

std::vector<std::uint8_t> FileReader::read(std::uint64_t offset, std::size_t len, std::string& err) {
    std::vector<std::uint8_t> buf(len);
    if (!read(offset, buf.data(), len, err)) {
        buf.clear();
    }
    return buf;
}

}  // namespace qtsvfs
