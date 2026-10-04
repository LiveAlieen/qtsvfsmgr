#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace qtsvfs {

// GlobalIndexPrime/Override.data 的**缓存格式**（不是包内流）：
//   [0x38 头][u32 fileHash[numFiles]][u16 packageId[numFiles]]
// 实测 56 + numFiles*4 + numFiles*2 恰等于 GlobalIndexPrime.data 的字节数。
// fileHash 升序，查询用二分。包名区不在本文件里（在 builtin 包 DB 的
// hash("(qts-global-node-index)") 条目内）。
struct GlobalIndexHeader {
    std::uint32_t magic = 0;
    std::uint32_t formatVersion = 0;
    std::uint32_t ver = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t buildId = 0;
    std::uint64_t internalPackageHash = 0;
    std::uint32_t numPackages = 0;
    std::uint32_t numFiles = 0;
    std::uint32_t numConflictFiles = 0;
    std::uint32_t compressMethod = 0;
    std::uint32_t compressedDataSize = 0;
    std::uint32_t reserved1 = 0;
};

class GlobalIndex {
public:
    bool load(const std::filesystem::path& path, std::string& err);

    const GlobalIndexHeader& header() const noexcept { return header_; }
    std::size_t fileCount() const noexcept { return hashes_.size(); }

    // 找不到返回 0xFFFF。
    std::uint16_t findPackage(std::uint32_t fileHash) const;
    // 用 FileNode 的 u64 哈希试查（u32 数组可能取高/低 32 位，由调用方统计命中率）。
    std::uint16_t findPackageByNodeHash(std::uint64_t nodeHash, bool* fromLowBits) const;

private:
    GlobalIndexHeader header_{};
    std::vector<std::uint32_t> hashes_;
    std::vector<std::uint16_t> ids_;
};

}  // namespace qtsvfs
