#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace qtsvfs {

// 节点哈希 → 虚拟路径。路径来自包内明文自声明或官方 node-index，已带前导 '/'。
using NameTable = std::unordered_map<std::uint64_t, std::string>;

// 节点哈希 → 定名来源（tree / real:catalog / object:selfname|objname|objpath|ingamepath / named）。
// 「来源」决定这个名字凭什么成立：real 与 named 的路径部分过了 hash(路径)==节点键 的闸门，
// object 靠「声明就在本节点体内」，tree 是名字与键成对出现 —— 不能混为一谈。
// 名单由 qtsvfs::liveNames 现场重建，不再有外部名单文件。
using SourceTable = std::unordered_map<std::uint64_t, std::string>;

// 名字表里的路径来自包内明文，可能带 ".."、保留字符或超长段；落盘前一律规整，
// 保证结果在导出目录之内且 Windows 可用。
std::string safeRelative(const std::string& path);

// 同名消歧：体内自声明的资源名不保证唯一（包 8 一个包就 32 万个名字，重复最多的是
// TransparentDepthPostpass 这种着色器通道名），两个节点用同一条路径会互相覆盖。
// 按节点哈希升序处理，先来者保留原名，后来者在末段追加 ~<哈希前8位>，
// 结果与遍历顺序无关，可复现。
void makeUnique(NameTable& names);

}  // namespace qtsvfs
