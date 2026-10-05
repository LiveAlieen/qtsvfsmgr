#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "qtsvfs/Names.h"
#include "qtsvfs/Package.h"

namespace qtsvfs {

struct ExportResult {
    std::uint64_t files = 0, bytes = 0, failed = 0, renamed = 0, skipped = 0;
    bool cancelled = false;
    std::string error;
};

// 按名字表把包内文件解出来落盘，路径先经 safeRelative 规整。
// limit=0 表示不限；cancel 返回 true 就中断；progress 每个文件回调一次（供界面刷新）。
ExportResult exportPackage(Package& pkg, const NameTable& names,
                           const std::filesystem::path& outDir, std::uint64_t limit,
                           const std::function<bool()>& cancel,
                           const std::function<void(const ExportResult&)>& progress,
                           bool allowNameless = false);

// 导出单个节点到指定文件（界面上「另存为」用）。
bool exportOne(Package& pkg, std::uint64_t hash, const std::filesystem::path& dst,
               std::string& err);

struct ExportOptions {
    bool allowNameless = false;  // 未定名也落盘，路径退化成 [nameless]/xx/HEX
    bool skipObsolete = false;   // 丢掉 FileNode 自声明的废弃节点
    std::uint64_t limit = 0;     // 0 = 不限
    std::filesystem::path manifestPath;  // 非空则逐行写 hash/包/落盘路径/字节/状态
    std::filesystem::path statePath;     // 每导完一个包追加一行，重跑时可据此跳过
    bool normalizeSerialized = false;    // 把载荷改回原厂 SerializedFile（见 Normalize.h）
    int threads = 1;                     // <=0 表示按 CPU 数
    std::function<bool()> cancel;
};

struct ExportSummary {
    std::uint64_t files = 0, bytes = 0;
    std::uint64_t noName = 0, obsolete = 0, noData = 0, decodeFailed = 0, shortBlob = 0;
    std::uint64_t renamed = 0, dirsFailed = 0, duplicate = 0, writeFailed = 0, normalized = 0;
    std::uint64_t resumed = 0, pending = 0;  // 续跑跳过的包数 / 本轮真正要导的包数
    bool cancelled = false;
    std::string error;
};

// 全量导出：所有包共写一棵输出树，包与包一个线程一个地并行解（ExportOptions::threads）。
// 同一个节点键可以出现在多个包里（全库 1,710,782 个节点 / 1,685,951 个唯一键），
// 所以导出过的键要记住，第二个包不再重解；跨包撞名同理需要一个贯穿全程的 used 集合，
// 两者都在锁里，真正的读块/解压/写盘在锁外。
// statePath 已存在时是续跑：跳过里面记下的包，并把旧 manifest 里的键和名字吃回来，
// 于是一个包不会解两遍、同名文件也不会被再加上尾巴。
ExportSummary exportAll(const std::vector<std::filesystem::path>& dirs, const NameTable& names,
                        const std::filesystem::path& outDir, const ExportOptions& opt,
                        const std::function<void(const ExportSummary&, const std::string& pkgName)>&
                            progress);

}  // namespace qtsvfs
