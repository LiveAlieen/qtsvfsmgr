#include "qtsvfs/Tree.h"

#include <algorithm>
#include <cinttypes>

namespace qtsvfs {
namespace {

std::string hexU64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::uint8_t methodOf(const FileNode& node) {
    return node.blocks.empty() ? 0 : static_cast<std::uint8_t>(node.blocks[0].packed & 0xFFu);
}

TreeNode* descend(TreeNode* cur, const std::string& path, bool leafNamed, const FileNode& node,
                  TreeStats& stats, const std::string& source) {
    std::size_t i = 0;
    std::string acc;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') {
            ++i;
        }
        std::size_t j = i;
        while (j < path.size() && path[j] != '/') {
            ++j;
        }
        if (j == i) {
            break;
        }
        const std::string seg = path.substr(i, j - i);
        acc += "/" + seg;
        auto it = std::find_if(cur->kids.begin(), cur->kids.end(),
                               [&](const TreeNode& k) { return k.name == seg; });
        if (it == cur->kids.end()) {
            TreeNode n;
            n.name = seg;
            n.path = acc;
            n.dir = true;
            cur->kids.push_back(std::move(n));
            it = cur->kids.end() - 1;
            ++stats.dirs;
        }
        cur = &*it;
        i = j;
    }
    cur->dir = false;
    cur->named = leafNamed;
    cur->source = source;
    cur->hash = node.hash;
    cur->size = node.size;
    cur->method = methodOf(node);
    cur->path = path;
    return cur;
}

}  // namespace

void buildTree(const std::vector<const FileNode*>& nodes, const NameTable& names, TreeNode& root,
               TreeStats& stats, const SourceTable* sources) {
    root.name = "";
    root.path = "";
    root.dir = true;
    root.kids.clear();
    for (const FileNode* node : nodes) {
        const auto it = names.find(node->hash);
        const bool named = it != names.end();
        const std::string path = named ? it->second : "/[nameless]/" + hexU64(node->hash);
        std::string source;
        if (named && sources) {
            const auto s = sources->find(node->hash);
            if (s != sources->end()) {
                source = s->second;
            }
        }
        descend(&root, path, named, *node, stats, source);
        ++stats.files;
        stats.bytes += node->size;
        (named ? stats.named : stats.nameless)++;
    }
}

void sortTree(TreeNode& node) {
    std::sort(node.kids.begin(), node.kids.end(), [](const TreeNode& a, const TreeNode& b) {
        if (a.dir != b.dir) {
            return a.dir;
        }
        return a.name < b.name;
    });
    for (auto& k : node.kids) {
        sortTree(k);
    }
}

void collectRows(const TreeNode& node, std::vector<TreeRow>& rows, std::uint64_t depth,
                 std::uint64_t depthLimit) {
    for (const auto& k : node.kids) {
        if (k.dir) {
            if (depthLimit == 0 || depth + 1 <= depthLimit) {
                collectRows(k, rows, depth + 1, depthLimit);
            }
            continue;
        }
        rows.push_back({k.path, k.hash, k.size, k.method, k.named, k.source});
    }
}

void printTree(const TreeNode& node, std::FILE* out, std::uint64_t depth, std::uint64_t depthLimit,
               TreeStats& walked) {
    for (const auto& k : node.kids) {
        for (std::uint64_t i = 0; i < depth; ++i) {
            std::fputs("  ", out);
        }
        if (k.dir) {
            ++walked.dirs;
            std::fprintf(out, "%s/\n", k.name.c_str());
            if (depthLimit == 0 || depth + 1 < depthLimit) {
                printTree(k, out, depth + 1, depthLimit, walked);
            }
        } else {
            ++walked.files;
            if (!k.named) {
                ++walked.nameless;
            }
            std::fprintf(out, "%s  %s size=%" PRIu64 " m=%u%s\n", k.name.c_str(),
                         hexU64(k.hash).c_str(), static_cast<unsigned long long>(k.size),
                         static_cast<unsigned>(k.method), k.named ? "" : "  [nameless]");
        }
    }
}

}  // namespace qtsvfs
