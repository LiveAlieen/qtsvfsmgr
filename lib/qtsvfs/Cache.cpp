#include "qtsvfs/Cache.h"

#include <cstring>
#include <fstream>

#include "qtsvfs/Package.h"

namespace qtsvfs {
namespace {

constexpr std::uint32_t kLibMagic = 0x51564C42;
constexpr std::uint32_t kHarvestMagic = 0x51564856;
constexpr std::uint32_t kVersion = 1;

void writeU32(std::ostream& os, std::uint32_t v) { os.write(reinterpret_cast<const char*>(&v), 4); }
void writeU64(std::ostream& os, std::uint64_t v) { os.write(reinterpret_cast<const char*>(&v), 8); }
void writeI64(std::ostream& os, std::int64_t v) { os.write(reinterpret_cast<const char*>(&v), 8); }
void writeStr(std::ostream& os, const std::string& s) {
    writeU32(os, static_cast<std::uint32_t>(s.size()));
    os.write(s.data(), s.size());
}
void writePath(std::ostream& os, const std::filesystem::path& p) {
    auto u8 = p.generic_u8string();
    writeStr(os, std::string(u8.begin(), u8.end()));
}

bool readU32(std::istream& is, std::uint32_t& v) { return !!is.read(reinterpret_cast<char*>(&v), 4); }
bool readU64(std::istream& is, std::uint64_t& v) { return !!is.read(reinterpret_cast<char*>(&v), 8); }
bool readI64(std::istream& is, std::int64_t& v) { return !!is.read(reinterpret_cast<char*>(&v), 8); }
bool readStr(std::istream& is, std::string& s) {
    std::uint32_t len = 0;
    if (!readU32(is, len)) return false;
    s.resize(len);
    return len == 0 || !!is.read(s.data(), len);
}
bool readPath(std::istream& is, std::filesystem::path& p) {
    std::string s;
    if (!readStr(is, s)) return false;
    p = std::filesystem::path(std::u8string(s.begin(), s.end()));
    return true;
}

std::int64_t toCacheTime(std::filesystem::file_time_type t) {
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

std::filesystem::file_time_type fromCacheTime(std::int64_t v) {
    using dur = std::filesystem::file_time_type::duration;
    return std::filesystem::file_time_type(dur(v));
}

std::filesystem::file_time_type dirMtime(const std::filesystem::path& dir) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(dir, ec);
    if (ec) return {};
    return t;
}

}  // namespace

std::filesystem::path libCachePath(const std::filesystem::path& root) {
    return root / ".qtsvfs-cache" / "lib.cache";
}

std::filesystem::path harvestCachePath(const std::filesystem::path& root,
                                       const std::string& pkgKey) {
    return root / ".qtsvfs-cache" / ("harvest_" + pkgKey + ".cache");
}

bool saveLibCache(const std::filesystem::path& cacheFile,
                  const std::vector<std::filesystem::path>& roots,
                  const LibCacheData& data) {
    std::error_code ec;
    std::filesystem::create_directories(cacheFile.parent_path(), ec);
    std::ofstream os(cacheFile, std::ios::binary);
    if (!os) return false;

    writeU32(os, kLibMagic);
    writeU32(os, kVersion);

    writeU32(os, static_cast<std::uint32_t>(roots.size()));
    for (const auto& r : roots) writePath(os, r);

    writeU32(os, static_cast<std::uint32_t>(data.pkgs.size()));
    for (const auto& p : data.pkgs) {
        writePath(os, p.dir);
        writeStr(os, p.mount);
        writeStr(os, p.label);
        writeI64(os, toCacheTime(dirMtime(p.dir)));
    }

    writeU32(os, static_cast<std::uint32_t>(data.scope.size()));
    for (const auto& p : data.scope) {
        writePath(os, p.dir);
        writeStr(os, p.mount);
        writeStr(os, p.label);
    }

    writeU32(os, data.fullIndex ? 1 : 0);

    writeU32(os, static_cast<std::uint32_t>(data.keys.size()));
    for (auto h : data.keys) writeU64(os, h);

    writeU32(os, static_cast<std::uint32_t>(data.dirs.size()));
    for (const auto& [h, seg] : data.dirs) {
        writeU64(os, h);
        writeStr(os, seg);
    }

    return os.good();
}

bool loadLibCache(const std::filesystem::path& cacheFile,
                  const std::vector<std::filesystem::path>& roots,
                  LibCacheData& data) {
    std::ifstream is(cacheFile, std::ios::binary);
    if (!is) return false;

    std::uint32_t magic = 0, ver = 0;
    if (!readU32(is, magic) || magic != kLibMagic) return false;
    if (!readU32(is, ver) || ver != kVersion) return false;

    std::uint32_t nRoots = 0;
    if (!readU32(is, nRoots)) return false;
    std::vector<std::filesystem::path> cachedRoots(nRoots);
    for (std::uint32_t i = 0; i < nRoots; ++i) {
        if (!readPath(is, cachedRoots[i])) return false;
    }
    if (cachedRoots.size() != roots.size()) return false;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (cachedRoots[i].generic_u8string() != roots[i].generic_u8string()) return false;
    }

    std::uint32_t nPkgs = 0;
    if (!readU32(is, nPkgs)) return false;
    data.pkgs.resize(nPkgs);
    for (std::uint32_t i = 0; i < nPkgs; ++i) {
        std::int64_t mt = 0;
        if (!readPath(is, data.pkgs[i].dir)) return false;
        if (!readStr(is, data.pkgs[i].mount)) return false;
        if (!readStr(is, data.pkgs[i].label)) return false;
        if (!readI64(is, mt)) return false;
        if (mt != toCacheTime(dirMtime(data.pkgs[i].dir))) return false;
    }

    std::uint32_t nScope = 0;
    if (!readU32(is, nScope)) return false;
    data.scope.resize(nScope);
    for (std::uint32_t i = 0; i < nScope; ++i) {
        if (!readPath(is, data.scope[i].dir)) return false;
        if (!readStr(is, data.scope[i].mount)) return false;
        if (!readStr(is, data.scope[i].label)) return false;
    }

    std::uint32_t fi = 0;
    if (!readU32(is, fi)) return false;
    data.fullIndex = fi != 0;

    std::uint32_t nKeys = 0;
    if (!readU32(is, nKeys)) return false;
    std::vector<std::uint64_t> keys(nKeys);
    for (std::uint32_t i = 0; i < nKeys; ++i) {
        if (!readU64(is, keys[i])) return false;
    }
    data.keys.load(keys.begin(), keys.end());

    std::uint32_t nDirs = 0;
    if (!readU32(is, nDirs)) return false;
    data.dirs.clear();
    data.dirs.reserve(nDirs);
    for (std::uint32_t i = 0; i < nDirs; ++i) {
        std::uint64_t h = 0;
        std::string seg;
        if (!readU64(is, h)) return false;
        if (!readStr(is, seg)) return false;
        data.dirs[h] = seg;
    }

    return true;
}

bool saveHarvestCache(const std::filesystem::path& cacheFile,
                      const PackageEntry& entry) {
    std::error_code ec;
    std::filesystem::create_directories(cacheFile.parent_path(), ec);
    std::ofstream os(cacheFile, std::ios::binary);
    if (!os) return false;

    writeU32(os, kHarvestMagic);
    writeU32(os, kVersion);
    writeI64(os, toCacheTime(dirMtime(entry.pkg->dir())));

    writeU32(os, static_cast<std::uint32_t>(entry.names.size()));
    for (const auto& [h, path] : entry.names) {
        writeU64(os, h);
        writeStr(os, path);
    }

    writeU32(os, static_cast<std::uint32_t>(entry.sources.size()));
    for (const auto& [h, src] : entry.sources) {
        writeU64(os, h);
        writeStr(os, src);
    }

    writeU64(os, entry.stats.nodes);
    writeU64(os, entry.stats.decoded);
    writeU64(os, entry.stats.failed);
    writeU64(os, entry.stats.oversized);
    writeU64(os, entry.stats.noData);
    writeU64(os, entry.stats.treeFiles);
    writeU64(os, entry.stats.catalogEntries);
    writeU64(os, entry.stats.treeRows);
    writeU64(os, entry.stats.realRows);
    writeU64(os, entry.stats.objectRows);
    writeU64(os, entry.stats.namedRows);
    writeU64(os, entry.stats.bytes);

    return os.good();
}

bool loadHarvestCache(const std::filesystem::path& cacheFile,
                      Package& pkg,
                      PackageEntry& entry) {
    std::ifstream is(cacheFile, std::ios::binary);
    if (!is) return false;

    std::uint32_t magic = 0, ver = 0;
    if (!readU32(is, magic) || magic != kHarvestMagic) return false;
    if (!readU32(is, ver) || ver != kVersion) return false;

    std::int64_t mt = 0;
    if (!readI64(is, mt)) return false;
    if (mt != toCacheTime(dirMtime(pkg.dir()))) return false;

    entry.names.clear();
    std::uint32_t nNames = 0;
    if (!readU32(is, nNames)) return false;
    entry.names.reserve(nNames);
    for (std::uint32_t i = 0; i < nNames; ++i) {
        std::uint64_t h = 0;
        std::string path;
        if (!readU64(is, h)) return false;
        if (!readStr(is, path)) return false;
        entry.names[h] = std::move(path);
    }

    entry.sources.clear();
    std::uint32_t nSources = 0;
    if (!readU32(is, nSources)) return false;
    entry.sources.reserve(nSources);
    for (std::uint32_t i = 0; i < nSources; ++i) {
        std::uint64_t h = 0;
        std::string src;
        if (!readU64(is, h)) return false;
        if (!readStr(is, src)) return false;
        entry.sources[h] = std::move(src);
    }

    if (!readU64(is, entry.stats.nodes)) return false;
    if (!readU64(is, entry.stats.decoded)) return false;
    if (!readU64(is, entry.stats.failed)) return false;
    if (!readU64(is, entry.stats.oversized)) return false;
    if (!readU64(is, entry.stats.noData)) return false;
    if (!readU64(is, entry.stats.treeFiles)) return false;
    if (!readU64(is, entry.stats.catalogEntries)) return false;
    if (!readU64(is, entry.stats.treeRows)) return false;
    if (!readU64(is, entry.stats.realRows)) return false;
    if (!readU64(is, entry.stats.objectRows)) return false;
    if (!readU64(is, entry.stats.namedRows)) return false;
    if (!readU64(is, entry.stats.bytes)) return false;

    entry.nodes.reserve(pkg.nodes().size());
    for (const auto& [h, node] : pkg.nodes()) {
        (void)h;
        entry.nodes.push_back(&node);
    }
    buildTree(entry.nodes, entry.names, entry.root, entry.treeStats, &entry.sources);
    sortTree(entry.root);

    return true;
}

}  // namespace qtsvfs
