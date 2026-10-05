#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "qtsvfs/Names.h"
#include "qtsvfs/format/QtsfNode.h"

namespace qtsvfs {

class GlobalIndex;

struct TreeNode {
    std::string name;
    std::string path;  // 完整虚拟路径；目录以 '/' 结尾
    bool dir = false;
    bool named = false;
    std::string source;  // 定名来源，见 SourceTable；空=哈希退化
    std::uint64_t hash = 0;
    std::uint64_t size = 0;
    std::uint8_t method = 0;
    std::vector<TreeNode> kids;
};

struct TreeStats {
    std::uint64_t files = 0, dirs = 0, named = 0, nameless = 0, obsolete = 0, bytes = 0;
};

// 一行文件记录，用于导出文件树清单。
struct TreeRow {
    std::string path;
    std::uint64_t hash = 0;
    std::uint64_t size = 0;
    std::uint8_t method = 0;
    bool named = false;
    std::string source;
};

// 用名字表把包内节点还原成真实目录树。没定名的节点单独收进 [nameless] 桶，
// 不跟真名混在一起，覆盖率才看得见。sources 可为空，那样只留 named 布尔。
// manifest（可空）= 当前版本的 GlobalIndex：未定名且其 fileHash（节点键低 32 位）
// 不在清单里的，落进 [obsolete] 而不是 [nameless] —— 实测未定名节点只有 51.4% 在清单里，
// 而已定名节点有 90.6%，且这种缺失是文件级散布（整包过期的只占 0.4%），
// 所以那一半是原厂侧废弃的残留，跟「活着但取不到名字」不是一回事，得分开摆。
void buildTree(const std::vector<const FileNode*>& nodes, const NameTable& names, TreeNode& root,
               TreeStats& stats, const SourceTable* sources = nullptr,
               const GlobalIndex* manifest = nullptr);

// 按「目录在前、名字在后」稳定排序，供界面与文本输出用同一套顺序。
void sortTree(TreeNode& node);

// 摊平成文件行（含 [nameless] 里的），depthLimit 为 0 表示不限层级。
void collectRows(const TreeNode& node, std::vector<TreeRow>& rows, std::uint64_t depth,
                 std::uint64_t depthLimit);

// CLI 用的缩进文本树。
void printTree(const TreeNode& node, std::FILE* out, std::uint64_t depth, std::uint64_t depthLimit,
               TreeStats& walked);

}  // namespace qtsvfs
