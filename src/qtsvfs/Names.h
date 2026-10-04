#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace qtsvfs {

// 节点哈希 → 虚拟路径。路径来自包内明文自声明或官方 node-index，已带前导 '/'。
using NameTable = std::unordered_map<std::uint64_t, std::string>;

// 节点哈希 → 定名来源（real / selfname / objname / ingamepath / named）。
// 「来源」决定这个名字凭什么成立：real/named 过了 hash(路径)==节点键 的闸门，
// selfname/objname/ingamepath 靠「声明就在节点体内」，两者不能混为一谈。
using SourceTable = std::unordered_map<std::uint64_t, std::string>;

// 读名字表。三种行都接受：
//   HASH<TAB>PATH                                   （mergenames / tree 输出）
//   HASH<TAB>named<TAB><TAB>PATH                   （scan 输出）
//   HASH<TAB>real|object<TAB><子类><TAB>PATH        （scan 输出，带来源）
// 同一个哈希重复时保留先出现的那条，所以调用方应把权威来源排在前面。
bool loadNameTable(const std::filesystem::path& file, NameTable& out, std::string& err,
                   SourceTable* sources = nullptr);

// 名字表里的路径来自包内明文，可能带 ".."、保留字符或超长段；落盘前一律规整，
// 保证结果在导出目录之内且 Windows 可用。
std::string safeRelative(const std::string& path);

// 同名消歧：体内自声明的资源名不保证唯一（包 8 一个包就 32 万个名字，重复最多的是
// TransparentDepthPostpass 这种着色器通道名），两个节点用同一条路径会互相覆盖。
// 按节点哈希升序处理，先来者保留原名，后来者在末段追加 ~<哈希前8位>，
// 结果与遍历顺序无关，可复现。
void makeUnique(NameTable& names);

}  // namespace qtsvfs
