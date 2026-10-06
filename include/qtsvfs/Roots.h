#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qtsvfs {

// 一个包目录（元数据卷 + 数据卷），加上它属于哪个 VFS 实例。
// 本机三处布局都归一到这个结构：
//   QtsVFSCache/packages/<id>/            mount = "packages"
//   QtsVFSCache/MiniApp_<id>/0/           mount = "MiniApp_<id>"（每个小程序是独立 VFS 实例，
//                                         自己的 0.db + 0_0.db + Globalindex/GlobalIndexOverride.data）
//   APK assets/bin/Data/Package/builtin/<id>/  mount = "builtin"
// mount 必须分开记：节点键是路径哈希，两个实例里同一个键可以指向不同内容的文件，
// 混在一起去重就等于把一个实例的文件当成另一个实例的。
struct PackageRef {
    std::filesystem::path dir;
    std::string mount;  // 目录的直接父目录名
    std::string label;  // 界面与 manifest 用：packages 的包就是 "<id>"，其余是 "MiniApp_10617/0"

    bool operator<(const PackageRef& o) const {
        if (mount != o.mount) {
            return mount < o.mount;
        }
        return dir < o.dir;
    }
};

// 目录是不是一个包：含与目录同名的元数据卷 <stem>.db（Package::open 的判据）。
bool isPackageDir(const std::filesystem::path& dir);

// 把若干路径展开成包列表。路径本身是包就只收它；否则向下找 maxDepth 层
// （packages 根差一层、QtsVFSCache 根与 MiniApp 根差两层）。
// 不是包的目录（builtin、Globalindex、Merge、Watchdog、*.json 之类）自然被排除。
std::vector<PackageRef> discoverPackages(const std::vector<std::filesystem::path>& roots,
                                         std::size_t maxDepth = 2);

// 全库 64 位节点键集合：定名那道哈希闸门要用它，同包节点表不够 ——
// 声明路径的宿主节点常常在别的包里（实测 .bytes/.txt 就是这样）。
// 只读各包元数据卷，不碰数据卷，所以全库 1,748 个包也就几秒。
class KeySet {
public:
    // 并行回调用，进度计数在 worker 线程里跑，实现要保证自己线程安全。
    using Progress = std::function<void(std::size_t done, std::size_t total)>;

    void build(const std::vector<PackageRef>& pkgs, int threads, const Progress& progress = {});

    bool contains(std::uint64_t hash) const noexcept { return keys_.count(hash) != 0; }
    bool empty() const noexcept { return keys_.empty(); }
    std::size_t size() const noexcept { return keys_.size(); }

    using const_iterator = std::unordered_set<std::uint64_t>::const_iterator;
    const_iterator begin() const noexcept { return keys_.begin(); }
    const_iterator end() const noexcept { return keys_.end(); }

    template <class It>
    void load(It first, It last) {
        keys_.clear();
        keys_.insert(first, last);
    }

private:
    std::unordered_set<std::uint64_t> keys_;
};

// 同键的「哈希验证过的挂载目录」。见 Harvest.h 的 mergeRows：体内自声明的名字（object 层）
// 天生没有目录段，要借同键 named/real 路径里的目录，而那些路径常在别的包里。
using DirIndex = std::unordered_map<std::uint64_t, std::string>;

// 稳定的缓存键：包目录的 UTF-8 通用路径。
std::string pathKey(const std::filesystem::path& p);

}  // namespace qtsvfs
