#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

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

}  // namespace qtsvfs
