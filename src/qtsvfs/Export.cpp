#include "qtsvfs/Export.h"

#include <algorithm>
#include <fstream>
#include <unordered_set>

namespace qtsvfs {
namespace {

std::string hexU64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

ExportResult exportPackage(Package& pkg, const NameTable& names, const std::filesystem::path& outDir,
                           std::uint64_t limit, const std::function<bool()>& cancel,
                           const std::function<void(const ExportResult&)>& progress) {
    ExportResult res;
    std::vector<std::pair<std::uint64_t, const FileNode*>> jobs;
    for (const auto& [h, node] : pkg.nodes()) {
        if (names.count(h)) {
            jobs.emplace_back(h, &node);
        }
    }
    // 按哈希排序，保证两次导出的落盘顺序与重名改名结果一致
    std::sort(jobs.begin(), jobs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::unordered_set<std::string> used;
    for (const auto& [h, node] : jobs) {
        if (limit && res.files >= limit) {
            break;
        }
        if (cancel && cancel()) {
            res.cancelled = true;
            break;
        }
        std::string rel = safeRelative(names.at(h));
        if (rel.empty()) {
            rel = hexU64(h);
        }
        if (!used.insert(rel).second) {
            rel += "#" + hexU64(h).substr(8, 8);  // 规整后撞名：加哈希尾巴，不覆盖已导出的
            ++res.renamed;
        }
        std::vector<std::uint8_t> blob;
        std::string err;
        if (!pkg.readBlob(*node, blob, err)) {
            ++res.failed;
            res.error = err;
            continue;
        }
        const std::filesystem::path dst = outDir / rel;
        std::error_code ec;
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::ofstream os(dst, std::ios::binary | std::ios::trunc);
        if (!os) {
            ++res.failed;
            res.error = "写入失败: " + dst.string();
            continue;
        }
        os.write(reinterpret_cast<const char*>(blob.data()),
                 static_cast<std::streamsize>(blob.size()));
        ++res.files;
        res.bytes += blob.size();
        if (progress) {
            progress(res);
        }
    }
    res.skipped = pkg.nodes().size() - jobs.size();
    return res;
}

bool exportOne(Package& pkg, std::uint64_t hash, const std::filesystem::path& dst, std::string& err) {
    const auto it = pkg.nodes().find(hash);
    if (it == pkg.nodes().end()) {
        err = "包内无该节点";
        return false;
    }
    std::vector<std::uint8_t> blob;
    if (!pkg.readBlob(it->second, blob, err)) {
        return false;
    }
    std::ofstream os(dst, std::ios::binary | std::ios::trunc);
    if (!os) {
        err = "写入失败: " + dst.string();
        return false;
    }
    os.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    return true;
}

}  // namespace qtsvfs
