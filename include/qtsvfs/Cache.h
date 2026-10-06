#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "qtsvfs/Harvest.h"
#include "qtsvfs/Roots.h"

namespace qtsvfs {

// 把 openLibrary 的产出（pkgs/keys/dirs）落到盘上，下次同目录直接读回，
// 跳过 discoverPackages + KeySet::build + buildDirIndex（全库索引最慢，要解所有块）。
// 失效条件：根路径变了、包目录 mtime 变了。
struct LibCacheData {
    std::vector<PackageRef> pkgs;
    std::vector<PackageRef> scope;
    KeySet keys;
    DirIndex dirs;
    bool fullIndex = false;
};

bool saveLibCache(const std::filesystem::path& cacheFile,
                  const std::vector<std::filesystem::path>& roots,
                  const LibCacheData& data);

bool loadLibCache(const std::filesystem::path& cacheFile,
                  const std::vector<std::filesystem::path>& roots,
                  LibCacheData& data);

// 把单包 harvest 结果（names/sources/stats）落盘。加载时 Package 仍要重新打开
// （只读元数据卷，快），但跳过 harvestPackage 的全块解压扫描。
// 失效条件：包目录 mtime 变了。
bool saveHarvestCache(const std::filesystem::path& cacheFile,
                      const PackageEntry& entry);

bool loadHarvestCache(const std::filesystem::path& cacheFile,
                      Package& pkg,
                      PackageEntry& entry);

// 按根目录算缓存路径：<root>/.qtsvfs-cache/lib.cache
std::filesystem::path libCachePath(const std::filesystem::path& root);

// 按包键算缓存路径：<root>/.qtsvfs-cache/harvest_<key>.cache
std::filesystem::path harvestCachePath(const std::filesystem::path& root,
                                       const std::string& pkgKey);

}  // namespace qtsvfs
