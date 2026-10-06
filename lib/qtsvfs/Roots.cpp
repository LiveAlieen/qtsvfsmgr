#include "qtsvfs/Roots.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_map>

#include "qtsvfs/Package.h"

namespace qtsvfs {
namespace {

// 包目录名一律是 ASCII（数字包号、MiniApp_<id>、builtin），直接 string() 就够。
std::string leaf(const std::filesystem::path& p) { return p.filename().string(); }

std::string parentName(const std::filesystem::path& dir) {
    if (!dir.has_parent_path()) {
        return {};
    }
    return leaf(dir.parent_path());
}

PackageRef makeRef(const std::filesystem::path& dir) {
    PackageRef ref;
    ref.dir = dir;
    ref.mount = parentName(dir);
    // packages 的包目录名本身就是包号，界面上够了；小程序/APK 那些包目录都叫 0，
    // 不带父目录就分不出是哪个实例。
    ref.label = ref.mount == "packages" ? leaf(dir) : ref.mount + "/" + leaf(dir);
    return ref;
}

}  // namespace

bool isPackageDir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    return std::filesystem::exists(dir / (dir.stem().wstring() + L".db"), ec);
}

std::vector<PackageRef> discoverPackages(const std::vector<std::filesystem::path>& roots,
                                         std::size_t maxDepth) {
    std::unordered_map<std::filesystem::path, PackageRef> found;
    std::vector<std::filesystem::path> frontier;
    for (const auto& r : roots) {
        frontier.push_back(r);
    }
    for (std::size_t depth = 0; depth <= maxDepth && !frontier.empty(); ++depth) {
        std::vector<std::filesystem::path> next;
        for (const auto& dir : frontier) {
            if (isPackageDir(dir)) {
                found.emplace(dir, makeRef(dir));
                continue;  // 包目录里不会再套包
            }
            if (depth == maxDepth) {
                continue;
            }
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (e.is_directory(ec)) {
                    next.push_back(e.path());
                }
            }
        }
        frontier = std::move(next);
    }
    std::vector<PackageRef> out;
    out.reserve(found.size());
    for (const auto& [dir, ref] : found) {
        (void)dir;
        out.push_back(ref);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string pathKey(const std::filesystem::path& p) {
    const auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

void KeySet::build(const std::vector<PackageRef>& pkgs, int threads,
                   const Progress& progress) {
    keys_.clear();
    if (pkgs.empty()) {
        return;
    }
    if (threads <= 0) {
        threads = static_cast<int>(std::thread::hardware_concurrency());
    }
    threads = std::max(1, std::min(threads, 32));

    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::vector<std::vector<std::uint64_t>> perThread(static_cast<std::size_t>(threads));
    auto worker = [&](std::size_t slot) {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= pkgs.size()) {
                break;
            }
            Package pkg;
            std::string err;
            if (pkg.open(pkgs[i].dir, err) && pkg.loadNodes(err)) {
                perThread[slot].reserve(perThread[slot].size() + pkg.nodes().size());
                for (const auto& [h, node] : pkg.nodes()) {
                    (void)node;
                    perThread[slot].push_back(h);
                }
            }
            if (progress) {
                progress(++done, pkgs.size());
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads - 1));
    for (int i = 1; i < threads; ++i) {
        pool.emplace_back(worker, static_cast<std::size_t>(i));
    }
    worker(0);
    for (auto& t : pool) {
        t.join();
    }
    std::size_t total = 0;
    for (const auto& v : perThread) {
        total += v.size();
    }
    keys_.reserve(total * 2);
    for (const auto& v : perThread) {
        keys_.insert(v.begin(), v.end());
    }
}

}  // namespace qtsvfs
