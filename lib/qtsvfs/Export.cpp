#include "qtsvfs/Export.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "qtsvfs/Normalize.h"

namespace qtsvfs {
namespace {

std::string hexU64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

// 未定名节点按哈希前两位分桶：全库有几十万节点定不下名，堆在同一个 [nameless] 目录里
// NTFS 枚举会明显变慢，AssetRipper 那边也不好按批处理。
std::string namelessRelative(std::uint64_t h) {
    const std::string hex = hexU64(h);
    return "[nameless]/" + hex.substr(0, 2) + "/" + hex;
}

// VFS 的规范路径是全小写的，所以贴图/音频的外部流节点看起来叫 `.ress`，而它的原厂名字是
// Unity 的 `.resS`：引用它的那个 SerializedFile 里 m_StreamData.path 写的就是
// `assets/00/<哈希>.resS`，AssetRipper 按这个串直接拼磁盘路径找文件。大小写还原回去，
// 下游才拿得到纹理像素（实测包 101 有 550 处「资源未找到」全是这个后缀造成的）。
void restoreStreamExtension(std::string& rel) {
    constexpr std::string_view lower = ".ress";
    constexpr std::string_view real = ".resS";
    if (rel.size() >= lower.size() && rel.compare(rel.size() - lower.size(), lower.size(), lower) == 0) {
        rel.replace(rel.size() - lower.size(), lower.size(), real);
    }
}

}  // namespace

ExportResult exportPackage(Package& pkg, const NameTable& names, const std::filesystem::path& outDir,
                           std::uint64_t limit, const std::function<bool()>& cancel,
                           const std::function<void(const ExportResult&)>& progress,
                           bool allowNameless) {
    ExportResult res;
    std::vector<std::pair<std::uint64_t, const FileNode*>> jobs;
    for (const auto& [h, node] : pkg.nodes()) {
        if (names.count(h) || allowNameless) {
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
        const auto ni = names.find(h);
        // 两条分支都必须过 safeRelative：以 '/' 开头的串会被 path::operator/= 当成
        // 绝对路径替换掉 outDir，实测把 1240 个未定名文件写到了盘根 [nameless]\ 下。
        std::string rel = safeRelative(ni == names.end() ? "[nameless]/" + hexU64(h) : ni->second);
        if (rel.empty()) {
            rel = hexU64(h);
        }
        restoreStreamExtension(rel);
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

namespace {

// 跨包共享的状态：一棵输出树的名字占用、已经解过的键、计数器和两个日志文件。
// 一个包一个线程，所以这些都要过锁；重活（读块、解压、写盘）都在锁外做。
struct ExportSession {
    const ExportOptions* opt = nullptr;
    std::filesystem::path outDir;
    std::mutex mx;
    std::ofstream manifest;
    std::ofstream state;
    std::unordered_set<std::string> used;
    // 已解过的键按 mount 分开记：节点键是路径哈希，两个 VFS 实例里同键可以指向
    // 不同内容的文件，共用一个集合就会把小程序的文件当成主缓存已经导过了。
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> done;
    std::unordered_map<std::string, std::string> mountOfLabel;  // 续跑时从 manifest 还原 mount
    bool prefixMounts = false;  // 输入里多于一个 mount 才加顶层目录
    ExportSummary total;
    std::atomic<bool> cancelled{false};
};

// 续跑时先吃回上一轮的 manifest：已落盘的键不再重解，已占用的名字不再加尾巴。
// 解压失败/没落盘的行不算占用（那条键还没导出过）。
void seedFromManifest(ExportSession& s) {
    std::ifstream in(s.opt->manifestPath, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t t1 = line.find('\t');
        if (t1 == std::string::npos) {
            continue;
        }
        const std::size_t t2 = line.find('\t', t1 + 1);
        const std::size_t t3 = line.find('\t', t2 + 1);
        const std::size_t t4 = line.find('\t', t3 + 1);
        if (t2 == std::string::npos || t3 == std::string::npos) {
            continue;
        }
        const std::string status = t4 == std::string::npos ? line.substr(t3 + 1) : line.substr(t4 + 1);
        if (status == "decode-fail") {
            continue;
        }
        const std::uint64_t h = std::strtoull(line.substr(0, t1).c_str(), nullptr, 16);
        const std::string pkgName = line.substr(t1 + 1, t2 - t1 - 1);
        const std::string rel = line.substr(t2 + 1, t3 - t2 - 1);
        if (!h || rel.empty()) {
            continue;
        }
        const auto m = s.mountOfLabel.find(pkgName);
        s.done[m == s.mountOfLabel.end() ? pkgName : m->second].insert(h);
        s.used.insert(rel);
    }
}

std::string dirKey(const std::filesystem::path& p) {
    const auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 单个包：打开卷、实时收割本包名单、解节点、落盘。
// 共享状态只在占名字、记数、写 manifest 时短暂上锁。
void exportOnePackage(ExportSession& s, const PackageRef& ref,
                      const std::function<void(const ExportSummary&, const std::string&)>& progress) {
    Package pkg;
    std::string err;
    if (!pkg.open(ref.dir, err) || !pkg.loadNodes(err)) {
        std::lock_guard<std::mutex> lk(s.mx);
        ++s.total.dirsFailed;
        s.total.error = err;  // 只留最后一条：全库跑起来前面几百条会把这行挤没
        return;
    }
    // 名单就在导出前现算：同一个 Package 对象的块索引两轮都用，不重开卷。
    NameTable names;
    HarvestStats hs;
    HarvestOptions ho;
    ho.keyset = s.opt->keyset;
    ho.prefixes = s.opt->prefixes;
    liveNames(pkg, ho, names, nullptr, &hs, nullptr, s.opt->dirs);
    const std::string mount = ref.mount;
    const std::string& pkgName = ref.label;  // manifest 与进度里都用它，跨实例不会混
    const std::string prefix = s.prefixMounts ? mount + "/" : std::string();
    {
        std::lock_guard<std::mutex> lk(s.mx);
        s.total.totalNodes += hs.nodes;
        s.total.namedNodes += names.size();
    }
    std::vector<std::pair<std::uint64_t, const FileNode*>> jobs;
    {
        std::lock_guard<std::mutex> lk(s.mx);
        const auto& done = s.done[mount];
        for (const auto& [h, node] : pkg.nodes()) {
            if (!done.count(h)) {
                jobs.emplace_back(h, &node);
            }
        }
    }
    std::sort(jobs.begin(), jobs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const ExportOptions& opt = *s.opt;
    for (const auto& [h, node] : jobs) {
        if (s.cancelled.load()) {
            return;
        }
        if (opt.skipObsolete && node->obsolete != 0) {
            std::lock_guard<std::mutex> lk(s.mx);
            ++s.total.obsolete;
            continue;
        }
        const auto ni = names.find(h);
        if (ni == names.end() && !opt.allowNameless) {
            std::lock_guard<std::mutex> lk(s.mx);
            ++s.total.noName;
            continue;
        }
        // 两条分支都必须过 safeRelative：以 '/' 开头的串会被 path::operator/= 当成
        // 绝对路径替换掉 outDir，实测把 1240 个未定名文件写到了盘根 [nameless]\ 下。
        std::string rel = ni == names.end() ? namelessRelative(h) : safeRelative(ni->second);
        if (rel.empty()) {
            rel = hexU64(h);
        }
        restoreStreamExtension(rel);
        rel = prefix + rel;  // 多实例导入时按 mount 分顶层目录，前缀不需要规整
        if (!pkg.dataPresent(h)) {
            std::lock_guard<std::mutex> lk(s.mx);
            ++s.total.noData;
            continue;  // 不记 done：同一个键在别的包里可能真有字节
        }
        std::vector<std::uint8_t> blob;
        std::string readErr;
        if (!pkg.readBlob(*node, blob, readErr)) {
            std::lock_guard<std::mutex> lk(s.mx);
            ++s.total.decodeFailed;
            s.total.error = readErr;
            if (s.manifest) {
                s.manifest << hexU64(h) << '\t' << pkgName << '\t' << rel << "\t0\tdecode-fail\n";
            }
            continue;
        }
        const bool shortBlob = blob.size() != node->size;
        // 归一化必须在长度核对之后做：它会合法地让载荷变短 4×类型数。
        const bool normalized = opt.normalizeSerialized && normalizeStrippedSerialized(blob);
        {
            // 占名要等到真要写盘的时候：块不全的节点不配把名字占掉，否则后面那个真有数据的
            // 同键包只能带 #哈希 尾巴落盘。
            std::lock_guard<std::mutex> lk(s.mx);
            if (!s.used.insert(rel).second) {
                rel += "#" + hexU64(h).substr(8, 8);  // 规整后撞名：加哈希尾巴，不覆盖已导出的
                ++s.total.renamed;
            }
        }
        const std::filesystem::path dst = s.outDir / rel;
        std::error_code ec;
        // 另一个节点的名字是以这个路径当目录用的（`hero/X` 和 `hero/X/part` 同时存在），
        // 这时文件写不下去，给文件名加哈希尾巴换个位置。
        // 注意：不能用 status 的 ec 判断——文件不存在时它也会置 ENOENT。
        const std::filesystem::file_status st = std::filesystem::status(dst, ec);
        ec.clear();
        if (std::filesystem::is_directory(st)) {
            std::lock_guard<std::mutex> lk(s.mx);
            rel += "#" + hexU64(h).substr(8, 8);
            if (s.used.insert(rel).second) {
                ++s.total.renamed;
            }
        }
        const std::filesystem::path file = s.outDir / rel;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream os(file, std::ios::binary | std::ios::trunc);
        if (!os) {
            std::lock_guard<std::mutex> lk(s.mx);
            ++s.total.writeFailed;
            s.total.error = "写入失败: " + file.string();
            s.used.erase(rel);  // 没落盘就不算占用，重跑还能补上
            continue;
        }
        os.write(reinterpret_cast<const char*>(blob.data()),
                 static_cast<std::streamsize>(blob.size()));
        {
            std::lock_guard<std::mutex> lk(s.mx);
            ++s.total.files;
            s.total.bytes += blob.size();
            s.done[mount].insert(h);
            if (shortBlob) {
                ++s.total.shortBlob;  // 拼出来的长度和节点自声明不符：本机块不全，下游多半解析不了
            }
            if (normalized) {
                ++s.total.normalized;
            }
            if (s.manifest) {
                s.manifest << hexU64(h) << '\t' << pkgName << '\t' << rel << '\t' << blob.size()
                           << (shortBlob ? "\tshort" : "\tfile") << '\n';
            }
        }
        if (opt.limit) {
            std::lock_guard<std::mutex> lk(s.mx);
            if (s.total.files >= opt.limit) {
                s.cancelled.store(true);
                return;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(s.mx);
        if (s.state) {
            s.state << "done\t" << dirKey(ref.dir) << '\n';
            s.state.flush();  // 崩了也要留下"这个包完了"的事实，否则续跑会重解
        }
        if (progress) {
            progress(s.total, pkgName);
        }
    }
}

}  // namespace

// 全量导出：包与包之间互不依赖，一个线程一个包并行跑，锁只盖住命名占用和计数。
ExportSummary exportAll(const std::vector<PackageRef>& pkgs,
                        const std::filesystem::path& outDir, const ExportOptions& opt,
                        const std::function<void(const ExportSummary&, const std::string&)>& progress) {
    ExportSession s;
    s.outDir = outDir;
    s.opt = &opt;
    std::size_t mounts = 0;
    {
        std::unordered_set<std::string> seen;
        for (const auto& ref : pkgs) {
            s.mountOfLabel.emplace(ref.label, ref.mount);
            if (seen.insert(ref.mount).second) {
                ++mounts;
            }
        }
    }
    s.prefixMounts = mounts > 1;  // 只导一个实例时保持原来的树形不变
    std::unordered_set<std::string> finished;
    const bool resume = !opt.statePath.empty() && std::filesystem::exists(opt.statePath);
    if (resume) {
        std::ifstream in(opt.statePath, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t tab = line.find('\t');
            if (tab != std::string::npos && line.compare(0, tab, "done") == 0) {
                finished.insert(line.substr(tab + 1));
            }
        }
    }
    if (!opt.manifestPath.empty()) {
        if (resume) {
            s.manifest.open(opt.manifestPath, std::ios::binary | std::ios::app);
            seedFromManifest(s);
        } else {
            s.manifest.open(opt.manifestPath, std::ios::binary | std::ios::trunc);
            s.manifest << "hash\tpackage\tpath\tbytes\tstatus\n";
        }
    }
    if (!opt.statePath.empty()) {
        s.state.open(opt.statePath, std::ios::binary | (resume ? std::ios::app : std::ios::trunc));
    }

    std::vector<const PackageRef*> todo;
    todo.reserve(pkgs.size());
    for (const auto& ref : pkgs) {
        if (!finished.count(dirKey(ref.dir))) {
            todo.push_back(&ref);
        }
    }
    int threads = opt.threads;
    if (threads <= 0) {
        threads = static_cast<int>(std::thread::hardware_concurrency());
    }
    threads = std::max(1, std::min(threads, 64));
    s.total.resumed = pkgs.size() - todo.size();
    s.total.pending = todo.size();

    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        while (!s.cancelled.load()) {
            if (opt.cancel && opt.cancel()) {
                s.cancelled.store(true);
                break;
            }
            const std::size_t i = next.fetch_add(1);
            if (i >= todo.size()) {
                break;
            }
            exportOnePackage(s, *todo[i], progress);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(std::max(1, threads - 1)));
    for (int i = 1; i < threads; ++i) {
        pool.emplace_back(worker);
    }
    worker();  // 主线程也干一份，省一次线程创建
    for (auto& t : pool) {
        t.join();
    }
    s.total.cancelled = s.cancelled.load();
    return s.total;
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
