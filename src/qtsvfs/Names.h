#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace qtsvfs {

// 节点哈希 → 虚拟路径。路径来自包内明文自声明或官方 node-index，已带前导 '/'。
using NameTable = std::unordered_map<std::uint64_t, std::string>;

// 读名字表。两种行都接受：
//   HASH<TAB>PATH                                   （tree 输出）
//   HASH<TAB>named<TAB><TAB>PATH                    （scan 输出，第三字段留空）
// 同一个哈希重复时保留先出现的那条，所以调用方应把权威来源排在前面。
bool loadNameTable(const std::filesystem::path& file, NameTable& out, std::string& err);

// 名字表里的路径来自包内明文，可能带 ".."、保留字符或超长段；落盘前一律规整，
// 保证结果在导出目录之内且 Windows 可用。
std::string safeRelative(const std::string& path);

}  // namespace qtsvfs
