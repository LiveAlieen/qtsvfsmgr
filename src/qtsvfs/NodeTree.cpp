#include "qtsvfs/NodeTree.h"

#include <cstring>

namespace qtsvfs {
namespace {

constexpr std::size_t kMaxName = 1024;
constexpr std::size_t kMaxEntries = 3'000'000;
constexpr int kMaxDepth = 64;

// MemoryStream 的读取语义：每次取数前都检查 pos+n <= size，越界即失败。
struct Reader {
    const std::uint8_t* b;
    std::size_t size;
    std::size_t pos = 0;

    bool want(std::size_t n) {
        const std::size_t aligned = n;
        if (aligned > size - pos) {
            return false;
        }
        return true;
    }
    void align(std::size_t n) {
        pos = (pos + n - 1) / n * n;
    }
    bool u8(std::uint8_t& v) {
        if (!want(1)) {
            return false;
        }
        v = b[pos];
        pos += 1;
        return true;
    }
    bool u32(std::uint32_t& v) {
        align(4);
        if (!want(4)) {
            return false;
        }
        std::memcpy(&v, b + pos, 4);
        pos += 4;
        return true;
    }
    bool u64(std::uint64_t& v) {
        align(8);
        if (!want(8)) {
            return false;
        }
        std::memcpy(&v, b + pos, 8);
        pos += 8;
        return true;
    }
    bool name(std::string& out) {
        std::uint32_t len = 0;
        if (!u32(len) || len > kMaxName || !want(len)) {
            return false;
        }
        for (std::uint32_t i = 0; i < len; ++i) {
            const std::uint8_t c = b[pos + i];
            if (c < 0x20 || c > 0x7e) {
                return false;  // 名字必须是可打印 ASCII，越界的都是撞上的巧合字节
            }
        }
        out.assign(reinterpret_cast<const char*>(b + pos), len);
        pos += len;
        return true;
    }
};

bool walk(Reader& r, std::string path, std::vector<NodeTreeEntry>& out, int depth) {
    std::uint8_t isDir = 0;
    if (depth > kMaxDepth || !r.u8(isDir)) {
        return false;
    }
    std::string nm;
    if (!r.name(nm)) {
        return false;
    }
    if ((isDir & 1) == 0) {
        std::uint64_t h = 0;
        if (!r.u64(h)) {
            return false;
        }
        if (out.size() >= kMaxEntries) {
            return false;
        }
        out.push_back({h, path + "/" + nm});
        return true;
    }
    path += nm.empty() ? "" : "/" + nm;
    std::uint32_t cnt = 0;
    if (!r.u32(cnt)) {
        return false;
    }
    // 每个子节点至少占 1(isDir)+4(nameLen)+4(childCount 或 8 对齐的 hash) 字节，
    // 用它挡掉「把随机二进制里的数字当成数量」这类误判。
    if (cnt > (r.size - r.pos) / 9) {
        return false;
    }
    for (std::uint32_t i = 0; i < cnt; ++i) {
        if (!walk(r, path, out, depth + 1)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool parseNodeTree(const std::uint8_t* data, std::size_t size, std::vector<NodeTreeEntry>& out) {
    // 根必须是 isDir=1 且名字为空：实测 8816 字节那份索引就是这样开头的 8 个字节。
    if (size < 16 || data[0] != 1 || data[1] != 0 || data[2] != 0 || data[3] != 0 ||
        data[4] != 0 || data[5] != 0 || data[6] != 0 || data[7] != 0) {
        return false;
    }
    out.clear();
    Reader r{data, size};
    if (!walk(r, "", out, 0) || out.empty()) {
        return false;
    }
    return r.pos == size;  // 正好读完才算真是这棵树，多一字节少一字节都是巧合
}

}  // namespace qtsvfs
