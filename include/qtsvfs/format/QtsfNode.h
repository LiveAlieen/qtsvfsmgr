#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qtsvfs {

// packages/<id>/<id>.db 里 value=56(+12n) 字节的条目。字段语义由实测 227,428 条单块
// 记录与 34 条多块记录反推：value 尺寸恒等于 44 + 12*blockCount。
struct QtsfBlock {
    std::uint32_t startPos = 0;   // 该块在 uncompressedSize 内的起始位置
    std::uint32_t packed = 0;     // 压缩参数打包值（低字节像 method id，见 QtsfCompressFlag）
    std::uint32_t blockSize = 0;  // 该块未压缩字节数（观测值 0x40000 / 0x4b000）
};

struct FileNode {
    // 原厂废弃标记。全库 1,674,850 个节点实测：0 有 1,339,450 个（80.0%），其中 98.4% 能在
    // 当前 GlobalIndex 里找到、83.1% 已被定名；1 有 335,394 个（20.0%），只有 7.5% 还在清单里。
    // 包 2/8/9/101/5165 分别复验过，方向一致 —— 这是 VFS 自己维护的逐节点墓碑位，
    // 比「拿 fileHash 去清单里查不到」的推断更硬，且不依赖清单文件（小程序根也能用）。
    std::uint32_t obsolete = 0;
    std::uint32_t version = 0;  // 观测如 0x00010016
    std::uint64_t hash = 0;     // 与记录 key 相同
    std::uint64_t size = 0;     // 未压缩总大小
    std::uint64_t checkA = 0;
    std::uint64_t checkB = 0;
    std::uint32_t blockCount = 0;
    std::vector<QtsfBlock> blocks;

    bool parse(const std::uint8_t* value, std::size_t len);
};

// /(qts-exportsetting-node-index).data 的递归节点流：
// [u8 dir][补到4][u32 名长][名][补到4] 目录→[u32 子数][子节点内联递归]；文件→[补到8][u64 hash]
struct QtsfNode {
    bool dir = false;
    std::string name;
    std::uint64_t hash = 0;
    std::vector<QtsfNode> children;
};

bool parseQtsfNode(const std::uint8_t* data, std::size_t len, QtsfNode& out, int depth = 0);

// 把树摊平成 hash -> 路径（根路径带前导 '/'，与 VFS_CalcHashCode64 的输入约定一致）。
void collectHashPaths(const QtsfNode& node, const std::string& prefix,
                      std::vector<std::pair<std::uint64_t, std::string>>& out);

}  // namespace qtsvfs
