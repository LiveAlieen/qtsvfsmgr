#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qtsvfs {

// 把 QtsVFS 重打包的 SerializedFile 改回原厂形态。
//
// 原厂 2022.3（FormatVersion 22）发布构建直接把 EnableTypeTree 写成 0，按自带类结构解析；
// 而重打包后的载荷留着 EnableTypeTree=1，类型树节点数组却是空的，而且空树后面不写字符串
// 缓冲区长度和类型依赖。AssetRipper 与 UnityPy 都会在这里多吃/少吃字节，之后整段元数据
// 错位（UnityPy 直接抛 TypeTreeNode.parse_blob 越界）。删掉每类型那个空的节点数 u32、
// 把 ett 置 0，并同步头里的 MetadataSize/FileSize/DataOffset，通用解析器就能正常读。
//
// 只处理"所有类型树都为空且 FileSize 与真实长度相符"的载荷；有真类型树、或本机块不全
// 导致截断的文件一律原样保留（返回 false），不做半吊子改写。
bool normalizeStrippedSerialized(std::vector<std::uint8_t>& blob);

}  // namespace qtsvfs
