#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qtsvfs {

std::string hexdump(const std::uint8_t* data, std::size_t len, std::uint64_t base_offset);

struct EntropyStats {
    double overall = 0.0;
    double block_min = 0.0;
    double block_max = 0.0;
    double block_mean = 0.0;
    std::uint64_t block_bytes = 0;
    std::size_t block_count = 0;
    // 熵 > 7.5 bit/byte 的块占比：压缩或加密数据的指示器。
    double high_entropy_ratio = 0.0;
};

EntropyStats measureEntropy(const std::vector<std::uint8_t>& data, std::uint64_t block_bytes);

struct U32ColumnScan {
    std::size_t in_range = 0;      // 值 < 文件大小，可作为文件内偏移
    std::size_t total = 0;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> samples;
};

U32ColumnScan scanU32AsOffsets(const std::uint8_t* data, std::size_t len,
                               std::uint64_t base_offset, std::uint64_t file_size,
                               std::size_t max_samples);

}  // namespace qtsvfs
