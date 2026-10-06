#include "qtsvfs/Normalize.h"

#include <cstring>

namespace qtsvfs {
namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint64_t be64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

void putBE32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

void putBE64(std::uint8_t* p, std::uint64_t v) {
    putBE32(p, static_cast<std::uint32_t>(v >> 32));
    putBE32(p + 4, static_cast<std::uint32_t>(v));
}

}  // namespace

bool normalizeStrippedSerialized(std::vector<std::uint8_t>& blob) {
    const std::size_t n = blob.size();
    if (n < 0x44 || be32(blob.data() + 8) != 22) {
        return false;  // 不是 gen-22 SerializedFile
    }
    if (be64(blob.data() + 0x18) != n) {
        return false;  // 本机块不全的截断载荷，改完也读不出对象
    }
    const std::uint32_t metaSize = be32(blob.data() + 0x14);
    const std::uint64_t dataOffset = be64(blob.data() + 0x20);
    std::size_t p = 0x30;
    while (p < n && blob[p] != 0) {
        ++p;
    }
    if (p >= n) {
        return false;
    }
    const std::size_t ettOff = p + 1 + 4;  // 版本串 + target platform
    if (ettOff >= n || blob[ettOff] != 1) {
        return false;  // 已经是 ett=0，或根本不是这个布局
    }
    std::size_t o = ettOff + 1;
    const std::uint32_t typeCount = le32(blob.data() + o);
    o += 4;
    if (typeCount == 0 || typeCount > 4096) {
        return false;
    }
    std::vector<std::pair<std::size_t, std::size_t>> spans;  // 每个类型的 [起始, 空节点数字段位置)
    spans.reserve(typeCount);
    for (std::uint32_t i = 0; i < typeCount; ++i) {
        if (o + 24 > n) {
            return false;
        }
        const std::size_t start = o;
        const std::int32_t rawId = static_cast<std::int32_t>(le32(blob.data() + o));
        o += 4;
        ++o;  // stripped
        const std::int16_t scriptIndex = static_cast<std::int16_t>(blob[o] | (blob[o + 1] << 8));
        o += 2;
        if (rawId == -1 || rawId == 114 || scriptIndex >= 0) {
            o += 16;  // ScriptID
        }
        o += 16;  // OldTypeHash
        if (o + 4 > n || le32(blob.data() + o) != 0) {
            return false;  // 有真类型树，通用解析器本来就能读，不动
        }
        spans.emplace_back(start, o);
        o += 4;
    }

    const std::size_t drop = 4 * static_cast<std::size_t>(typeCount);
    std::vector<std::uint8_t> out;
    out.reserve(n - drop);
    out.insert(out.end(), blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(ettOff));
    out.push_back(0);  // EnableTypeTree -> 0，和原厂发布构建一致
    out.insert(out.end(), blob.begin() + static_cast<std::ptrdiff_t>(ettOff + 1),
               blob.begin() + static_cast<std::ptrdiff_t>(spans.front().first));
    for (std::size_t i = 0; i < spans.size(); ++i) {
        // 类型条目是连续的：[start, cut) 是要留的，cut..cut+4 是那个空的节点数字段，删掉。
        out.insert(out.end(), blob.begin() + static_cast<std::ptrdiff_t>(spans[i].first),
                   blob.begin() + static_cast<std::ptrdiff_t>(spans[i].second));
    }
    out.insert(out.end(), blob.begin() + static_cast<std::ptrdiff_t>(o), blob.end());
    if (out.size() != n - drop) {
        return false;
    }
    putBE32(out.data() + 0x14, metaSize - static_cast<std::uint32_t>(drop));
    putBE64(out.data() + 0x18, out.size());
    putBE64(out.data() + 0x20, dataOffset - drop);
    blob.swap(out);
    return true;
}

}  // namespace qtsvfs
