#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qtsvfs {

// exporter::Node::ReadFromStream 的序列化布局（libQtsVFS.so.c 第 242524 行起，逐字段照抄）：
//   u8 isDir;            pos += 1
//   对齐到 4 → u32 nameLen → nameLen 字节名字
//   isDir==0：对齐到 8 → u64 hash        （日志串 "exporter::Node::ReadFromStream t.hash failed"）
//   isDir==1：对齐到 4 → u32 childCount → 递归 childCount 个子节点（"children[%d] failed"）
//
// 这份 dump 里 /(qts-exportsetting-node-index).data（节点 AF1FB540A2B8CB52，8816 字节）就是它，
// 实测按上式解析正好吃掉 8816/8816 字节、出 210 个文件节点，且每个 u64 都等于该文件的节点键
// （AF1FB540A2B8CB52、1D13F7315D89BAC3、A9D3F37EDF8E13CC…），所以这是**权威的 hash→完整路径表**：
// 名字与键在同一条记录里成对出现，不需要再靠哈希反推。
struct NodeTreeEntry {
    std::uint64_t hash = 0;
    std::string path;  // 以 '/' 分隔的完整路径，含前导 '/'
};

// 只认「根是 isDir 且名字为空」的整棵树；任何一处越界、或没有正好消耗完输入，都返回 false。
bool parseNodeTree(const std::uint8_t* data, std::size_t size, std::vector<NodeTreeEntry>& out);

}  // namespace qtsvfs
