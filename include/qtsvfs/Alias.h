#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace qtsvfs {

struct AliasResult {
    std::uint64_t total = 0;     // 导出树里的文件总数
    std::uint64_t scanned = 0;   // 已扫过的文件数
    std::uint64_t refs = 0;      // 读到的外部流引用条数（含重复）
    std::uint64_t hit = 0;       // 引用路径哈希命中 manifest 节点的
    std::uint64_t linked = 0;
    std::uint64_t already = 0;   // 规范名已经在盘上（名字表本来就给对了）
    std::uint64_t stale = 0;     // 命中了但目标文件不在盘上（无字节或在别的包里）
    std::uint64_t failed = 0;
    std::string error;
};

// 把 SerializedFile 里 m_StreamData.path 写的规范路径接到导出树上。
//
// 为什么需要这一步：`.resS` 流节点在 VFS 里是按 `assets/xx/<32位十六进制>.resS` 寻址的，
// 而这个键的**规范路径**只能从引用它的那个 SerializedFile 里读到。名字表给这些节点的多半是
// catalog 里的显示名（`assets/ffPJD_M_xxx_High.ress` 这种，连分隔符都是拼出来的），
// 于是 AssetRipper 按 m_StreamData 报的名字找不到流，纹理全部导不出来。
// 引用路径经 calcHashCode64("/" + 小写) 就是节点键，manifest 第一列也是节点键，
// 所以两边能一一对上——给已经导出的文件补一个硬链接名字即可，不占额外空间。
//
// refsPath 非空时另外写出 `宿主文件相对路径<TAB>流相对路径`：分片喂 AssetRipper 时要把
// SerializedFile 和它引用的流放在同一片，否则流又找不到了，这份边表就是分片的依据。
AliasResult linkStreamAliases(const std::filesystem::path& root, const std::filesystem::path& manifest,
                              bool dryRun, int threads, const std::filesystem::path& refsPath,
                              const std::function<void(const AliasResult&)>& progress);

}  // namespace qtsvfs
