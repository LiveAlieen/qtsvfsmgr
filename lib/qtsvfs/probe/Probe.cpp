#include "qtsvfs/probe/Probe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qtsvfs {
namespace {

double shannon(const std::uint8_t* p, std::size_t len) {
    if (len == 0) {
        return 0.0;
    }
    std::uint32_t hist[256] = {};
    for (std::size_t i = 0; i < len; ++i) {
        ++hist[p[i]];
    }
    double bits = 0.0;
    const double inv = 1.0 / static_cast<double>(len);
    for (std::uint32_t c : hist) {
        if (c == 0) {
            continue;
        }
        const double pr = static_cast<double>(c) * inv;
        bits -= pr * std::log2(pr);
    }
    return bits;
}

std::string toHex(std::uint64_t v, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*llX", width * 2, static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

std::string hexdump(const std::uint8_t* data, std::size_t len, std::uint64_t base_offset) {
    std::string out;
    out.reserve(len * 4 + 64);
    for (std::size_t off = 0; off < len; off += 16) {
        out += toHex(base_offset + off, 8);
        out += "  ";
        std::string ascii;
        for (std::size_t i = 0; i < 16; ++i) {
            if (off + i < len) {
                const std::uint8_t b = data[off + i];
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%02X ", b);
                out += buf;
                ascii += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
            } else {
                out += "   ";
                ascii += ' ';
            }
            if (i == 7) {
                out += ' ';
            }
        }
        out += " |" + ascii + "|\n";
    }
    return out;
}

EntropyStats measureEntropy(const std::vector<std::uint8_t>& data, std::uint64_t block_bytes) {
    EntropyStats s;
    s.block_bytes = block_bytes;
    if (data.empty() || block_bytes == 0) {
        return s;
    }
    const double overall_bits = shannon(data.data(), data.size());
    s.overall = overall_bits;

    double sum = 0.0;
    std::size_t count = 0;
    std::size_t high = 0;
    s.block_min = 8.0;
    for (std::size_t off = 0; off + block_bytes <= data.size(); off += block_bytes) {
        const double e = shannon(data.data() + off, block_bytes);
        s.block_min = std::min(s.block_min, e);
        s.block_max = std::max(s.block_max, e);
        sum += e;
        ++count;
        if (e > 7.5) {
            ++high;
        }
    }
    s.block_count = count;
    if (count > 0) {
        s.block_mean = sum / static_cast<double>(count);
        s.high_entropy_ratio = static_cast<double>(high) / static_cast<double>(count);
    } else {
        s.block_min = overall_bits;
        s.block_max = overall_bits;
        s.block_mean = overall_bits;
    }
    return s;
}

U32ColumnScan scanU32AsOffsets(const std::uint8_t* data, std::size_t len,
                               std::uint64_t base_offset, std::uint64_t file_size,
                               std::size_t max_samples) {
    U32ColumnScan scan;
    for (std::size_t i = 0; i + 4 <= len; i += 4) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                (static_cast<std::uint32_t>(data[i + 2]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 3]) << 24);
        ++scan.total;
        if (v != 0xFFFFFFFF && v < file_size) {
            ++scan.in_range;
            if (scan.samples.size() < max_samples) {
                scan.samples.emplace_back(base_offset + i, v);
            }
        }
    }
    return scan;
}

}  // namespace qtsvfs
