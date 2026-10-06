#include "qtsvfs/Harvest.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>

#include "qtsvfs/Cache.h"
#include "qtsvfs/Package.h"

namespace qtsvfs {

std::size_t Library::mounts() const {
    std::unordered_map<std::string, std::size_t> seen;
    for (const auto& ref : pkgs) {
        ++seen[ref.mount];
    }
    return seen.size();
}

std::string Library::labelFor(const std::filesystem::path& dir) const {
    const std::string key = pathKey(dir);
    for (const auto& ref : pkgs) {
        if (pathKey(ref.dir) == key) {
            return ref.label;
        }
    }
    return dir.filename().string();
}

Library openLibrary(const std::vector<std::filesystem::path>& roots, int threads, bool withKeyset,
                    bool withDirs,
                    const std::function<void(const std::string&, std::size_t, std::size_t)>& progress,
                    const std::filesystem::path& cachePath) {
    Library lib;

    if (!cachePath.empty()) {
        LibCacheData cd;
        if (loadLibCache(cachePath, roots, cd)) {
            lib.pkgs = std::move(cd.pkgs);
            lib.scope = std::move(cd.scope);
            lib.keys = std::move(cd.keys);
            lib.dirs = std::move(cd.dirs);
            lib.fullIndex = cd.fullIndex;
            if (!lib.pkgs.empty()) {
                return lib;
            }
        }
    }

    lib.pkgs = discoverPackages(roots);
    if (lib.pkgs.empty()) {
        return lib;
    }
    lib.fullIndex = withKeyset || withDirs;
    std::vector<std::filesystem::path> scopeRoots;
    for (const auto& r : roots) {
        scopeRoots.push_back(isPackageDir(r) ? r.parent_path() : r);
    }
    lib.scope = discoverPackages(scopeRoots);
    if (withKeyset) {
        lib.keys.build(lib.scope, threads, [&](std::size_t done, std::size_t total) {
            if (progress) {
                progress("节点键", done, total);
            }
        });
    }
    if (withDirs) {
        buildDirIndex(lib.scope, withKeyset && !lib.keys.empty() ? &lib.keys : nullptr, {}, threads,
                      lib.dirs, [&](std::size_t done, std::size_t total) {
                          if (progress) {
                              progress("目录段", done, total);
                          }
                      });
    }

    if (!cachePath.empty()) {
        LibCacheData cd;
        cd.pkgs = lib.pkgs;
        cd.scope = lib.scope;
        cd.keys = lib.keys;
        cd.dirs = lib.dirs;
        cd.fullIndex = lib.fullIndex;
        saveLibCache(cachePath, roots, cd);
    }

    return lib;
}

std::shared_ptr<PackageEntry> openPackageEntry(Library& lib, const PackageRef& ref,
                                               const std::filesystem::path& cachePath) {
    const std::string key = pathKey(ref.dir);
    if (const auto hit = lib.opened.find(key); hit != lib.opened.end()) {
        return hit->second;
    }

    auto entry = std::make_shared<PackageEntry>();
    entry->pkg = std::make_shared<Package>();
    std::string err;
    if (!entry->pkg->open(ref.dir, err) || !entry->pkg->loadNodes(err)) {
        return nullptr;
    }

    if (!cachePath.empty()) {
        if (loadHarvestCache(cachePath, *entry->pkg, *entry)) {
            lib.opened.emplace(key, entry);
            return entry;
        }
    }

    entry->nodes.reserve(entry->pkg->nodes().size());
    for (const auto& [h, node] : entry->pkg->nodes()) {
        (void)h;
        entry->nodes.push_back(&node);
    }
    HarvestOptions ho;
    ho.keyset = lib.keys.empty() ? nullptr : &lib.keys;
    liveNames(*entry->pkg, ho, entry->names, &entry->sources, &entry->stats, nullptr,
              lib.dirs.empty() ? nullptr : &lib.dirs);
    buildTree(entry->nodes, entry->names, entry->root, entry->treeStats, &entry->sources);
    sortTree(entry->root);

    if (!cachePath.empty()) {
        saveHarvestCache(cachePath, *entry);
    }

    lib.opened.emplace(key, entry);
    return entry;
}

}  // namespace qtsvfs
