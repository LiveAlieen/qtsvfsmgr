#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "qtsvfs/Catalog.h"
#include "qtsvfs/Names.h"
#include "qtsvfs/Roots.h"
#include "qtsvfs/Tree.h"
#include "qtsvfs/format/QtsfNode.h"

namespace qtsvfs {

class Package;

// 一条定名候选。四列与 scan TSV 一致，tag 就是来源层，sub 是层内子类：
//   tree   节点树索引（exporter::Node 格式）：名字与节点键在同一条记录里成对出现，权威
//   real   catalog 记录：同一条记录里「资源真名 + 声明路径」，路径过了哈希闸门
//   object 节点体内自声明：selfname(体内只有一个名字) / objname(多个名字里择优) /
//          objpath(没有名字字段时退用 bundle 内部路径) / ingamepath(脚本运行时写的模块路径)
//   named  裸路径哈希定名：hash64('/'+小写(候选)) == 节点键；只给得出哈希路径，不是人话名字
// 前两层的配对依据是「声明就在本节点体内」，不是哈希闸门；只有 real 和 named 里的路径
// 部分过了闸门。混着看会把「可读」当成「已验证」。
struct NameRow {
    std::uint64_t hash = 0;
    std::string tag;
    std::string sub;
    std::string path;
};

struct HarvestStats {
    std::uint64_t nodes = 0, decoded = 0, failed = 0, oversized = 0, noData = 0;
    std::uint64_t treeFiles = 0, catalogEntries = 0;
    std::uint64_t treeRows = 0, realRows = 0, objectRows = 0, namedRows = 0;
    std::uint64_t bytes = 0;  // 解出的未压缩总字节
    std::unordered_map<std::string, std::uint64_t> failReason;
    // 「盘上无块」的节点清单（哈希, 声明长度）：全库 14.5% 的节点在本机一个字节都没有，
    // 报定名率必须把这个分母单列出来，否则听起来像还差 30% 的功力。
    std::vector<std::pair<std::uint64_t, std::uint64_t>> noDataNodes;
};

struct HarvestOptions {
    // 非 .resS 文件在明文里是相对挂载点写的，要补根前缀才命中节点键。
    std::vector<std::string> prefixes;
    // 跨包哈希闸门；空则只认同包节点（单包导入时就是这样）。
    const KeySet* keyset = nullptr;
    std::uint64_t maxNodeSize = 64ull * 1024 * 1024;  // 超过这么大的块不碰
    std::uint64_t maxNodes = 0;                        // 每包最多解多少节点，0=不限（采样用）
    std::size_t textLimit = 0;                         // 每个节点最多扫多少明文字节，0=不限
    // 只收 catalog/nodetree 的目录段，跳过 barePaths/inGamePath/selfName。
    // buildDirIndex 用：后三者的产物在目录索引里本来就被丢弃，barePaths 还最贵（逐字节扫）。
    bool dirsOnly = false;
};

// 默认挂载根：官方白名单树里出现的两个根 + 实测裸路径命中过的两个根 + /exportdata。
const std::vector<std::string>& defaultNamePrefixes();

// 解一遍包内所有节点的块，按四层来源出候选行。
// onBlob 非空则每个解码后的节点回调一次（scan --markers 用它找新的自声明格式）。
// bodyKinds 非空则记下每个节点的「体内有什么」（是否序列化文件、几个名字、几条路径），
// 供 bucketUnnamed 在合并完之后给未定名节点分桶 —— 必须等合并完，因为一个节点可能
// 由别的节点的 catalog 行定名。
void harvestPackage(Package& pkg, const HarvestOptions& opt, std::vector<NameRow>& rows,
                    HarvestStats& stats,
                    const std::function<void(const FileNode&, const std::vector<std::uint8_t>&)>&
                        onBlob = nullptr,
                    std::unordered_map<std::uint64_t, CatalogStats>* bodyKinds = nullptr);

// 并行收割所有包，只收 hash→目录段，不合并名单（省掉一半内存）。
// progress 在多 worker 里调，实现要自己保证线程安全。
void buildDirIndex(const std::vector<PackageRef>& pkgs, const KeySet* keyset,
                   const std::vector<std::string>& prefixes, int threads, DirIndex& out,
                   const std::function<void(std::size_t done, std::size_t total)>& progress = {});

// 候选行 → 名字表：优先级 tree > real > object > named 只留一条；
// object 层天生没有目录段，先用本批 named 行的目录借，再退到 extraDirs（全库索引）；
// 最后统一同名消歧（体内名字会重复，包 8 一个包 32 万个）。sources 可以不给。
void mergeRows(const std::vector<NameRow>& rows, NameTable& names, SourceTable* sources = nullptr,
               const DirIndex* extraDirs = nullptr);

// 未定名原因分桶。kindCount 里给的是「非序列化容器 magic=xxxx」「序列化但体内无名字字段」
// 「聚合 bundle(体内声明 N 条路径)」这类原因。
void bucketUnnamed(const std::unordered_map<std::uint64_t, CatalogStats>& bodyKinds,
                   const NameTable& names, std::unordered_map<std::string, std::uint64_t>& kinds);

// 实时重建一个包的名单：收割 + 合并 + 消歧。layout/export/界面都从这里拿名字表，
// 不再读任何外部名单文件。rows 非空时把候选行带出去（scan 要写 TSV）。
void liveNames(Package& pkg, const HarvestOptions& opt, NameTable& names,
               SourceTable* sources = nullptr, HarvestStats* stats = nullptr,
               std::vector<NameRow>* rows = nullptr, const DirIndex* extraDirs = nullptr);

// 一个已打开并现算过名单的包：卷句柄、节点指针数组、名单、来源、真实目录树。
// Package 与节点指针、树同生命周期，所以三样放在一起。
struct PackageEntry {
    std::shared_ptr<Package> pkg;
    std::vector<const FileNode*> nodes;
    NameTable names;
    SourceTable sources;
    HarvestStats stats;
    TreeNode root;
    TreeStats treeStats;
};

// 一次导入：要处理的包 + 两道全库索引 + 已经现算过的包。
// CLI（layout/scan/export）与界面共用，定名行为只在库里这一处。
struct Library {
    std::vector<PackageRef> pkgs;   // 要干活的包
    std::vector<PackageRef> scope;   // 建索引的范围（同一 VFS 实例的全部包）
    KeySet keys;                     // 哈希闸门用的全库节点键
    DirIndex dirs;                   // hash→验证过的挂载目录
    std::unordered_map<std::string, std::shared_ptr<PackageEntry>> opened;  // pathKey → 现算结果
    bool fullIndex = false;           // 建过全库索引（false = 只有本包信息）

    std::size_t mounts() const;
    std::string labelFor(const std::filesystem::path& dir) const;
};

// 展开根并建两道索引。withKeyset / withDirs 关掉时退化成「只用本包信息」：快，
// 但跨包引用的路径定不出名、体内自声明的名字也没有目录段可借（实测包 101 约三成）。
// progress(stage, done, total) 在 worker 线程里调用，实现要自己保证线程安全。
// scopeFilter 非空时只把匹配的包放进 scope（建索引用），pkgs 不受影响。
Library openLibrary(const std::vector<std::filesystem::path>& roots, int threads, bool withKeyset,
                    bool withDirs,
                    const std::function<void(const std::string& stage, std::size_t done,
                                             std::size_t total)>& progress = {},
                    const std::filesystem::path& cachePath = {},
                    const std::function<bool(const PackageRef&)>& scopeFilter = {});

// 现算一个包的名单并建好目录树；同目录重复调用直接复用缓存。失败返回空指针。
// 树是全量的，界面那边按目录节点限量铺 item（超大包一个目录几万个文件时不至于卡死）。
std::shared_ptr<PackageEntry> openPackageEntry(Library& lib, const PackageRef& ref,
                                               const std::filesystem::path& cachePath = {});

}  // namespace qtsvfs
