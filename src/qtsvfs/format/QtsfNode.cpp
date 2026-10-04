#include "qtsvfs/format/QtsfNode.h"

#include <cctype>

#include "qtsvfs/format/QtsfStream.h"

namespace qtsvfs {
namespace {

constexpr int kMaxDepth = 64;
constexpr std::size_t kNodeHeader = 44;   // 4+4+8+8+8+8+4
constexpr std::size_t kBlockEntrySize = 12;

bool isLikelyPath(const std::string& s) {
    if (s.size() > 1024) {  // 根节点名是空串
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

bool parseNodeBody(QtsfStream& in, QtsfNode& node, int depth);

bool parseChildren(QtsfStream& in, QtsfNode& node, int depth) {
    const std::uint32_t count = in.u32();
    if (!in.ok() || count > 2000000) {
        return false;
    }
    node.children.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        QtsfNode child;
        if (!parseNodeBody(in, child, depth + 1)) {
            return false;
        }
        node.children.push_back(std::move(child));
    }
    return in.ok();
}

bool parseNodeBody(QtsfStream& in, QtsfNode& node, int depth) {
    if (depth > kMaxDepth) {
        return false;
    }
    const std::uint32_t dirWord = in.u32();
    node.dir = (dirWord & 1u) != 0;
    node.name = in.str();
    if (!in.ok() || !isLikelyPath(node.name)) {
        return false;
    }
    if (node.dir) {
        return parseChildren(in, node, depth);
    }
    node.hash = in.u64();
    return in.ok();
}

}  // namespace

bool FileNode::parse(const std::uint8_t* value, std::size_t len) {
    if (len < kNodeHeader) {
        return false;
    }
    QtsfStream in(value, len);
    zero0 = in.u32();
    version = in.u32();
    hash = in.u64();
    size = in.u64();
    checkA = in.u64();
    checkB = in.u64();
    blockCount = in.u32();
    if (!in.ok()) {
        return false;
    }
    if (len != kNodeHeader + static_cast<std::size_t>(blockCount) * kBlockEntrySize) {
        return false;
    }
    blocks.resize(blockCount);
    for (auto& b : blocks) {
        b.startPos = in.u32();
        b.packed = in.u32();
        b.blockSize = in.u32();
    }
    return in.ok();
}

bool parseQtsfNode(const std::uint8_t* data, std::size_t len, QtsfNode& out, int depth) {
    if (depth > kMaxDepth || len < 8) {
        return false;
    }
    QtsfStream in(data, len);
    return parseNodeBody(in, out, depth);
}

void collectHashPaths(const QtsfNode& node, const std::string& prefix,
                      std::vector<std::pair<std::uint64_t, std::string>>& out) {
    const std::string path = prefix + node.name;
    if (!node.dir) {
        out.emplace_back(node.hash, path);
        return;
    }
    for (const auto& child : node.children) {
        collectHashPaths(child, path + "/", out);
    }
}

}  // namespace qtsvfs
