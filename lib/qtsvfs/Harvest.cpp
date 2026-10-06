#include "qtsvfs/Harvest.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <string_view>
#include <thread>
#include <unordered_set>

#include "qtsvfs/NodeTree.h"
#include "qtsvfs/Package.h"
#include "qtsvfs/format/PathHash.h"

namespace qtsvfs {
namespace {

bool tokenChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '_' ||
           c == '-' || c == '.' || c == '/' || c == '+';
}

bool hasExtension(std::string_view t) {
    const std::size_t dot = t.rfind('.');
    if (dot == std::string::npos || dot + 1 >= t.size() || t.size() - dot - 1 > 8) {
        return false;
    }
    for (std::size_t k = dot + 1; k < t.size(); ++k) {
        if (!std::isalnum(static_cast<unsigned char>(t[k]))) {
            return false;
        }
    }
    return true;
}

std::string toLower(std::string_view s) {
    std::string o(s);
    for (char& c : o) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return o;
}

std::string collapseDoubleSlash(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '/' && k + 1 < s.size() && s[k + 1] == '/') {
            continue;
        }
        o += s[k];
    }
    return o;
}

// VFS 规范路径全小写（官方 210 条白名单用 hash64(小写路径) 校验 210/210 命中，保留大小写
// 只有 2 条），所以形态维度只剩「要不要折叠 //」与「挂哪个根」。
// 根前缀来自实测：容器明文里的 .bytes/.txt/.mp4/.png 都是相对挂载点写的，
// 补上 /RawAssets/Domestic 后 GlobalIndex 命中率从 0 跳到 200/201、166/182、47/64。
std::vector<std::string> buildForms(std::string_view tok, const std::vector<std::string>& prefixes) {
    std::vector<std::string> out;
    for (int v = 0; v < 2; ++v) {
        std::string base = v ? collapseDoubleSlash(tok) : std::string(tok);
        base = toLower(base);
        while (!base.empty() && base[0] == '/') {
            base.erase(base.begin());
        }
        for (const auto& p : prefixes) {
            std::string full = "/" + p + base;
            // 已经规范过的串，「原样」和「折叠」会算出同一个值，重复计数会让命中率虚高
            if (std::find(out.begin(), out.end(), full) != out.end()) {
                continue;
            }
            out.push_back(std::move(full));
        }
    }
    return out;
}

// 从明文里取路径候选：除了 ASCII 串，还要取 UTF-16LE（Windows/Wwise/Unity 序列化里
// 路径常以两字节字符存，中间夹 0x00，ASCII 扫描整段都会漏掉）。
template <class F>
void forEachPathToken(const std::vector<std::uint8_t>& blob, std::size_t limit, F&& f) {
    const std::size_t size =
        limit == 0 ? blob.size() : std::min(blob.size(), limit);
    const char* data = reinterpret_cast<const char*>(blob.data());
    std::size_t i = 0;
    while (i < size) {
        if (!tokenChar(data[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < size && tokenChar(data[j]) && j - i < 260) {
            ++j;
        }
        f(std::string_view(data + i, j - i));
        i = j;
    }
    i = 0;
    while (i + 1 < size) {
        if (!tokenChar(data[i]) || data[i + 1] != '\0') {
            ++i;
            continue;
        }
        std::string tok;
        std::size_t k = i;
        while (k + 1 < size && tokenChar(data[k]) && data[k + 1] == '\0' && tok.size() < 260) {
            tok += data[k];
            k += 2;
        }
        i = k > i ? k : i + 1;
        if (tok.size() >= 2 && tok.find('/') != std::string::npos) {
            f(std::string_view(tok));  // UTF-16 取出来是新串，生命周期到此为止
        }
    }
}

struct Run {
    const HarvestOptions& opt;
    Package& pkg;
    std::vector<NameRow>& rows;
    HarvestStats& stats;
    std::unordered_set<std::uint64_t> seenNamed, seenReal, seenTree, seenObject;

    void emit(std::uint64_t hash, const char* tag, const char* sub, std::string path) {
        rows.push_back(NameRow{hash, tag, sub, std::move(path)});
    }
};

// 裸路径候选：资源/脚本内容里直接写出的路径串（不依赖前面的键名）。
// 闸门是整 64 位节点键相等，噪声可以忽略；跨包的宿主节点靠 KeySet 才认得出来。
void harvestBarePaths(Run& r, const std::vector<std::uint8_t>& blob, const FileNode& node) {
    forEachPathToken(blob, r.opt.textLimit, [&](std::string_view tok) {
        if (tok.size() < 8 || tok.find('/') == std::string::npos) {
            return;
        }
        for (const std::string& cand : buildForms(tok, r.opt.prefixes)) {
            const std::uint64_t h = calcHashCode64(cand);
            // 自命中也算：那条路径就是本节点的规范路径，是 named 层最有用的产出
            const bool hit = h == node.hash || r.pkg.nodes().count(h) != 0 ||
                             (r.opt.keyset && r.opt.keyset->contains(h));
            if (!hit || !r.seenNamed.insert(h).second) {
                continue;
            }
            r.emit(h, "named", "", cand);
            ++r.stats.namedRows;
        }
    });
}

// catalog 记录：同一条记录里先出现资源真名、往后出现它的声明路径。
// 路径过了哈希闸门才把真名挂上去；目录段就用这条已过闸门的路径的目录 ——
// 它是哈希验证过的挂载位置，不必再去别的包里借。
void harvestCatalog(Run& r, const std::vector<std::uint8_t>& blob, const FileNode& node) {
    const auto entries = parseCatalog(blob.data(), blob.size());
    r.stats.catalogEntries += entries.size();
    for (const auto& e : entries) {
        std::string lower = toLower(e.path);
        while (!lower.empty() && lower[0] == '/') {
            lower.erase(lower.begin());
        }
        const std::uint64_t h = calcHashCode64("/" + lower);
        if (h != node.hash && r.pkg.nodes().count(h) == 0 &&
            !(r.opt.keyset && r.opt.keyset->contains(h))) {
            continue;  // 声明的路径在这份数据里没有对应节点，名字也就无处可挂
        }
        if (!r.seenReal.insert(h).second) {
            continue;
        }
        std::string real = e.name;
        for (char& c : real) {
            if (c == '/' || c == '\\') {
                c = '_';
            }
        }
        const std::size_t slash = lower.rfind('/');
        const std::string dir = slash == std::string::npos ? std::string() : lower.substr(0, slash);
        r.emit(h, "real", "catalog", "/" + (dir.empty() ? "" : dir + "/") + real + "." + toLower(e.ext));
        ++r.stats.realRows;
    }
}

// 节点树索引（exporter::Node 格式）：名字与节点键在同一条记录里成对出现，权威定名。
void harvestNodeTree(Run& r, const std::vector<std::uint8_t>& blob) {
    std::vector<NodeTreeEntry> entries;
    if (!parseNodeTree(blob.data(), blob.size(), entries)) {
        return;
    }
    ++r.stats.treeFiles;
    for (const auto& e : entries) {
        if (!r.seenTree.insert(e.hash).second) {
            continue;
        }
        r.emit(e.hash, "tree", "nodetree", e.path);
        ++r.stats.treeRows;
    }
}

// 脚本节点的自声明模块名：JS 运行时在文件头写 `InGamePath: JS/…/x.mjs`。
// 它哈希不到任何节点键 —— 那是把它当成「指向别人的路径」才有的要求；它其实是
// 「我这个文件叫什么」，配对依据是包含关系，不需要哈希闸门。
// 只有在整块明文里只出现 1 个不同取值时才采纳，多了就是引用列表。
bool harvestInGamePath(Run& r, const std::vector<std::uint8_t>& blob, const FileNode& node) {
    static const std::string key = "InGamePath:";
    std::string_view text(reinterpret_cast<const char*>(blob.data()),
                          r.opt.textLimit == 0 ? blob.size() : std::min(blob.size(), r.opt.textLimit));
    std::string val;
    std::size_t distinct = 0;
    for (std::size_t p = text.find(key); p != std::string_view::npos;
         p = text.find(key, p + key.size())) {
        std::size_t s = p + key.size();
        while (s < text.size() && text[s] == ' ') {
            ++s;
        }
        std::size_t e = s;
        while (e < text.size() && tokenChar(text[e]) && e - s < 240) {
            ++e;
        }
        std::string v(text.substr(s, e - s));
        if (v.size() < 4) {
            continue;
        }
        if (val.empty()) {
            val = std::move(v);
            ++distinct;
        } else if (val != v) {
            ++distinct;
            break;
        }
    }
    if (distinct != 1) {
        return false;
    }
    std::string name = collapseDoubleSlash(val);
    if (name.empty() || name[0] != '/') {
        name = "/" + name;
    }
    if (!r.seenObject.insert(node.hash).second) {
        return true;
    }
    r.emit(node.hash, "object", "ingamepath", std::move(name));
    ++r.stats.objectRows;
    return true;
}

// 单资源节点的体内自声明名字（selfname/objname/objpath，判据见 Catalog.h）。
void harvestSelfName(Run& r, const std::vector<std::uint8_t>& blob, const FileNode& node) {
    SelfName sn;
    if (!parseSelfName(blob.data(), blob.size(), sn)) {
        return;
    }
    if (!r.seenObject.insert(node.hash).second) {
        return;
    }
    const std::string path = "/" + (sn.dir.empty() ? "" : sn.dir + "/") + sn.name;
    r.emit(node.hash, "object",
           sn.fromPath ? "objpath" : sn.confident ? "selfname" : "objname", path);
    ++r.stats.objectRows;
}

}  // namespace

const std::vector<std::string>& defaultNamePrefixes() {
    // 空串＝不补根（.resS 这类本来就带全路径）；其余是实测命中过的挂载点。
    static const std::vector<std::string> kPrefixes = {
        "", "rawassets/domestic/", "rawassets/shared/", "exportdata/", "unity_buildin_payload/"};
    return kPrefixes;
}

void harvestPackage(Package& pkg, const HarvestOptions& options, std::vector<NameRow>& rows,
                    HarvestStats& stats,
                    const std::function<void(const FileNode&, const std::vector<std::uint8_t>&)>&
                        onBlob,
                    std::unordered_map<std::uint64_t, CatalogStats>* bodyKinds) {
    HarvestOptions opt = options;
    if (opt.prefixes.empty()) {
        // 默认挂载根在这里补一次，直调 harvestPackage 的命令（scan、目录索引预扫）
        // 才不会跟 liveNames 走出两套结果。
        opt.prefixes = defaultNamePrefixes();
    }
    Run r{opt, pkg, rows, stats};
    std::vector<const FileNode*> list;
    list.reserve(pkg.nodes().size());
    for (const auto& [h, node] : pkg.nodes()) {
        (void)h;
        list.push_back(&node);
    }
    // 按「小的先来 + 同尺寸按哈希」排，两次跑的顺序一致，重名消歧的结果才可复现。
    std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
        if (a->size != b->size) {
            return a->size < b->size;
        }
        return a->hash < b->hash;
    });

    std::vector<std::uint8_t> blob;
    for (const FileNode* node : list) {
        if (opt.maxNodes && stats.nodes >= opt.maxNodes) {
            break;  // 采样上限：只想看一个小包时不必解完
        }
        ++stats.nodes;
        if (node->size > opt.maxNodeSize) {
            ++stats.oversized;
            continue;
        }
        std::string err;
        try {
            if (!pkg.readBlob(*node, blob, err)) {
                ++stats.failed;
                const std::string reason = err.substr(0, 72);
                ++stats.failReason[reason];
                if (reason.find("没有数据卷") != std::string::npos) {
                    ++stats.noData;  // 这份按需下载的缓存里根本没它的字节
                    stats.noDataNodes.emplace_back(node->hash, node->size);
                }
                blob.clear();
                continue;
            }
        } catch (const std::exception& e) {
            ++stats.failed;
            ++stats.failReason[std::string("exception: ") + e.what()];
            blob.clear();
            continue;
        }
        ++stats.decoded;
        stats.bytes += blob.size();
        if (onBlob) {
            onBlob(*node, blob);
        }
        if (bodyKinds) {
            bodyKinds->try_emplace(node->hash, measureCatalog(blob.data(), blob.size()));
        }
        harvestNodeTree(r, blob);
        harvestBarePaths(r, blob, *node);
        harvestCatalog(r, blob, *node);
        // 顺序即优先级：脚本节点先看运行时自声明的模块路径，再看序列化资源名。
        if (!harvestInGamePath(r, blob, *node)) {
            harvestSelfName(r, blob, *node);
        }
        blob.clear();
    }
}

void mergeRows(const std::vector<NameRow>& rows, NameTable& names, SourceTable* sources,
               const DirIndex* extraDirs) {
    struct Best {
        int rank = 9;
        std::string path;
        std::string source;
    };
    std::unordered_map<std::uint64_t, Best> best;
    std::unordered_map<std::uint64_t, std::string> dirOf;
    best.reserve(rows.size());
    for (const auto& row : rows) {
        // tree 最优先：名字与节点键成对出现；其次 real（catalog 真名，路径过了闸门），
        // 再 object（体内自声明），最后 named（只有哈希路径）。同级先来者留。
        const int rank = row.tag == "tree"   ? 0
                         : row.tag == "real" ? 1
                         : row.tag == "object" ? 2 : 3;
        const std::string source = row.sub.empty() ? row.tag : row.tag + ":" + row.sub;
        if (row.tag == "named" || row.tag == "real") {
            // 这两条的路径部分过了哈希闸门，目录段就是验证过的挂载位置
            const std::size_t slash = row.path.rfind('/');
            if (slash != std::string::npos) {
                dirOf.try_emplace(row.hash, row.path.substr(0, slash));
            }
        }
        const auto it = best.find(row.hash);
        if (it == best.end()) {
            best.emplace(row.hash, Best{rank, row.path, source});
        } else if (rank < it->second.rank) {
            it->second = Best{rank, row.path, source};
        }
    }

    for (const auto& [h, b] : best) {
        if (b.rank > 2) {
            continue;  // named 本身就是完整路径，不需要借目录
        }
        auto it = best.find(h);
        std::string& path = it->second.path;
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash != 0) {
            continue;  // 这条名字已经带了目录段
        }
        const auto d = dirOf.find(h);
        if (d != dirOf.end()) {
            path = d->second + path;
            continue;
        }
        // 本批行里没有这个键的验证路径 —— 它多半写在别的包里，用全库索引补。
        if (extraDirs) {
            const auto g = extraDirs->find(h);
            if (g != extraDirs->end()) {
                path = g->second + path;
            }
        }
    }

    for (const auto& [h, b] : best) {
        names.emplace(h, b.path);
        if (sources) {
            sources->emplace(h, b.source);
        }
    }
    makeUnique(names);
}

void buildDirIndex(const std::vector<PackageRef>& pkgs, const KeySet* keyset,
                   const std::vector<std::string>& prefixes, int threads, DirIndex& out,
                   const std::function<void(std::size_t, std::size_t)>& progress) {
    if (pkgs.empty()) {
        return;
    }
    if (threads <= 0) {
        threads = static_cast<int>(std::thread::hardware_concurrency());
    }
    threads = std::max(1, std::min(threads, 32));
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::vector<DirIndex> perThread(static_cast<std::size_t>(threads));
    auto worker = [&](std::size_t slot) {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= pkgs.size()) {
                break;
            }
            Package pkg;
            std::string err;
            if (pkg.open(pkgs[i].dir, err) && pkg.loadNodes(err)) {
                HarvestOptions ho;
                ho.keyset = keyset;
                ho.prefixes = prefixes;
                std::vector<NameRow> rows;
                HarvestStats stats;
                harvestPackage(pkg, ho, rows, stats);
                DirIndex& local = perThread[slot];
                for (const auto& r : rows) {
                    if (r.tag != "named" && r.tag != "real") {
                        continue;
                    }
                    const std::size_t slash = r.path.rfind('/');
                    if (slash == std::string::npos) {
                        continue;
                    }
                    local.try_emplace(r.hash, r.path.substr(0, slash));
                }
            }
            if (progress) {
                progress(++done, pkgs.size());
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(std::max(1, threads - 1)));
    for (int i = 1; i < threads; ++i) {
        pool.emplace_back(worker, static_cast<std::size_t>(i));
    }
    worker(0);
    for (auto& t : pool) {
        t.join();
    }
    std::size_t total = 0;
    for (const auto& d : perThread) {
        total += d.size();
    }
    out.reserve(out.size() + total);
    for (const auto& d : perThread) {
        for (const auto& [h, dir] : d) {
            out.try_emplace(h, dir);
        }
    }
}

void bucketUnnamed(const std::unordered_map<std::uint64_t, CatalogStats>& bodyKinds,
                   const NameTable& names, std::unordered_map<std::string, std::uint64_t>& kinds) {
    for (const auto& [h, cs] : bodyKinds) {
        if (names.count(h)) {
            continue;
        }
        std::string kind;
        if (!cs.serialized) {
            kind = "非序列化容器 magic=" + cs.magic;
        } else if (cs.paths > 1) {
            kind = "聚合 bundle(体内声明 " + std::to_string(cs.paths) + " 条路径)";
        } else if (cs.names == 0) {
            kind = "序列化但体内无名字字段";
        } else {
            kind = "有名字却没被采纳(名字数 " + std::to_string(cs.names) + ")";
        }
        ++kinds[kind];
    }
}

void liveNames(Package& pkg, const HarvestOptions& opt, NameTable& names, SourceTable* sources,
               HarvestStats* stats, std::vector<NameRow>* rows, const DirIndex* extraDirs) {
    HarvestOptions full = opt;
    if (full.prefixes.empty()) {
        full.prefixes = defaultNamePrefixes();
    }
    std::vector<NameRow> local;
    HarvestStats localStats;
    harvestPackage(pkg, full, local, localStats);
    mergeRows(local, names, sources, extraDirs);
    if (stats) {
        *stats = localStats;
    }
    if (rows) {
        *rows = std::move(local);
    }
}

}  // namespace qtsvfs
