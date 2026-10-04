#include "qtsvfs/format/GlobalIndex.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace qtsvfs {
namespace {
constexpr std::uint32_t kHeaderSize = 0x38;
constexpr std::uint32_t kMagic = 0x51474E49;  // "INGQ"
constexpr std::uint16_t kNotFound = 0xFFFF;

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}
}  // namespace

bool GlobalIndex::load(const std::filesystem::path& path, std::string& err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        err = "打不开: " + path.string();
        return false;
    }
    const auto fileSize = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::uint8_t> buf(fileSize);
    if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(fileSize))) {
        err = "读取失败: " + path.string();
        return false;
    }
    if (fileSize < kHeaderSize) {
        err = "文件比头还短";
        return false;
    }
    const std::uint8_t* h = buf.data();
    header_.magic = le32(h);
    header_.formatVersion = le32(h + 4);
    header_.ver = le32(h + 8);
    header_.reserved0 = le32(h + 12);
    header_.buildId = static_cast<std::uint64_t>(le32(h + 16)) |
                      (static_cast<std::uint64_t>(le32(h + 20)) << 32);
    header_.internalPackageHash = static_cast<std::uint64_t>(le32(h + 24)) |
                                  (static_cast<std::uint64_t>(le32(h + 28)) << 32);
    header_.numPackages = le32(h + 32);
    header_.numFiles = le32(h + 36);
    header_.numConflictFiles = le32(h + 40);
    header_.compressMethod = le32(h + 44);
    header_.compressedDataSize = le32(h + 48);
    header_.reserved1 = le32(h + 52);
    if (header_.magic != kMagic) {
        err = "魔数不符: 0x" + std::to_string(header_.magic);
        return false;
    }
    const std::size_t need = static_cast<std::size_t>(header_.numFiles) * 6;
    if (fileSize - kHeaderSize < need) {
        err = "数组区小于 numFiles*6，可能不是缓存格式";
        return false;
    }
    hashes_.resize(header_.numFiles);
    ids_.resize(header_.numFiles);
    const std::uint8_t* p = buf.data() + kHeaderSize;
    for (std::uint32_t i = 0; i < header_.numFiles; ++i) {
        hashes_[i] = le32(p + i * 4);
    }
    const std::uint8_t* q = p + static_cast<std::size_t>(header_.numFiles) * 4;
    for (std::uint32_t i = 0; i < header_.numFiles; ++i) {
        ids_[i] = le16(q + i * 2);
    }
    return true;
}

std::uint16_t GlobalIndex::findPackage(std::uint32_t fileHash) const {
    auto it = std::lower_bound(hashes_.begin(), hashes_.end(), fileHash);
    if (it == hashes_.end() || *it != fileHash) {
        return kNotFound;
    }
    return ids_[static_cast<std::size_t>(it - hashes_.begin())];
}

std::uint16_t GlobalIndex::findPackageByNodeHash(std::uint64_t nodeHash, bool* fromLowBits) const {
    const std::uint32_t lo = static_cast<std::uint32_t>(nodeHash & 0xFFFFFFFFu);
    const std::uint32_t hi = static_cast<std::uint32_t>(nodeHash >> 32);
    std::uint16_t r = findPackage(lo);
    if (r != kNotFound) {
        if (fromLowBits) {
            *fromLowBits = true;
        }
        return r;
    }
    r = findPackage(hi);
    if (fromLowBits) {
        *fromLowBits = false;
    }
    return r;
}

}  // namespace qtsvfs
