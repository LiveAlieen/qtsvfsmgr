#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "qtsvfs/codec/Codec.h"
#include "qtsvfs/Alias.h"
#include "qtsvfs/Catalog.h"
#include "qtsvfs/Export.h"
#include "qtsvfs/Harvest.h"
#include "qtsvfs/Names.h"
#include "qtsvfs/NodeTree.h"
#include "qtsvfs/Roots.h"
#include "qtsvfs/Tree.h"
#include "qtsvfs/format/GlobalIndex.h"
#include "qtsvfs/format/Kdb.h"
#include "qtsvfs/format/QtsfNode.h"
#include "qtsvfs/Package.h"
#include "qtsvfs/format/PathHash.h"
#include "qtsvfs/io/FileReader.h"
#include "qtsvfs/probe/Probe.h"

extern "C" {
#include "lz4.h"
#include "zlib.h"
}
#include "ooz_wrapper.h"
#include "zstd.h"

namespace {

// Windows 下 argv 走 ANSI 码页，中文资源路径会坏掉，因此统一从宽字符命令行取参数，
// 只在匹配 ASCII 选项时转成 UTF-8。
std::string wideToUtf8(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}

// 选项里拿到的是 UTF-8 串，Windows 下要还原成宽字符路径才能开中文目录。
std::filesystem::path utf8ToPath(const std::string& s) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(s);
#endif
}

std::vector<std::wstring> readWideArgs(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int n = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::wstring> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        out.emplace_back(raw[i]);
    }
    LocalFree(raw);
    return out;
#else
    std::vector<std::wstring> out;
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(std::string(argv[i]).begin(), std::string(argv[i]).end());
    }
    return out;
#endif
}

bool parseSize(const char* s, std::uint64_t& out) {
    const std::size_t n = std::strlen(s);
    if (n == 0) {
        return false;
    }
    std::uint64_t mul = 1;
    const char u = static_cast<char>(std::tolower(static_cast<unsigned char>(s[n - 1])));
    if (u == 'k') mul = 1024ull;
    else if (u == 'm') mul = 1024ull * 1024;
    else if (u == 'g') mul = 1024ull * 1024 * 1024;
    const std::size_t digits = (mul == 1) ? n : n - 1;
    const std::string num(s, digits);
    char* end = nullptr;
    const long long v = std::strtoll(num.c_str(), &end, 0);
    if (end == num.c_str() || *end != '\0' || v < 0) {
        return false;
    }
    out = static_cast<std::uint64_t>(v) * mul;
    return true;
}

std::string toHexU64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::uint32_t le32At(const std::vector<std::uint8_t>& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::string bytesToHex(const std::uint8_t* p, std::size_t n) {
    std::string s;
    s.reserve(n * 2);
    char buf[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", p[i]);
        s += buf;
    }
    return s;
}

std::string toHexU32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

void printCodecs() {
    std::printf("zlib  %s\n", zlibVersion());
    std::printf("lz4   %d\n", LZ4_versionNumber());
    std::printf("zstd  %s\n", ZSTD_versionString());
    std::printf("ooz   %s\n", ooz::Version());
    std::printf("lz3   qtsvfs_lz3 (method 7 / 14)\n");
}

int cmdHash(const std::vector<std::filesystem::path>& files) {
    for (const auto& p : files) {
        const std::string s = wideToUtf8(p.native());
        // 与 VFS_CalcHashCode64 一致：对不含结尾 NUL 的路径字节做哈希。
        const std::uint64_t h = qtsvfs::calcHashCode64(s);
        std::printf("%016llX  %s\n", static_cast<unsigned long long>(h), s.c_str());
    }
    return 0;
}

bool openKdb(const std::filesystem::path& p, qtsvfs::FileReader& fr, qtsvfs::KdbFile& db,
             std::string& err) {
    if (!fr.open(p, err)) {
        return false;
    }
    return db.open(fr, err);
}

int cmdKdbInfo(const std::filesystem::path& p) {
    qtsvfs::FileReader fr;
    qtsvfs::KdbFile db;
    std::string err;
    if (!openKdb(p, fr, db, err)) {
        std::fprintf(stderr, "错误: %s (%s)\n", err.c_str(), wideToUtf8(p).c_str());
        return 2;
    }
    const auto& h = db.header();
    std::printf("file         %s\n", wideToUtf8(p).c_str());
    std::printf("size         %llu\n", static_cast<unsigned long long>(fr.size()));
    std::printf("magic        0x%s\n", toHexU64(h.magic).c_str());
    std::printf("usedSize     %u\n", h.usedSize);
    std::printf("commitSize   %u\n", h.commitSize);
    std::printf("rootPage     0x%s\n", toHexU32(h.rootPage).c_str());
    std::printf("rootPageAlt  0x%s\n", toHexU32(h.rootPageAlias).c_str());
    std::printf("chains       0x%s / 0x%s\n", toHexU32(h.chainA).c_str(), toHexU32(h.chainB).c_str());
    std::printf("toc          off=0x%s len=%u count=%u serial=%u\n", toHexU32(h.tocOffset).c_str(),
                h.tocLength, h.tocCount, h.tocSerial);
    std::printf("alloc        ");
    for (std::uint32_t v : h.alloc) {
        std::printf("0x%s ", toHexU32(v).c_str());
    }
    std::printf("\n");
    return 0;
}

int cmdKdbRecords(const std::filesystem::path& p, std::uint64_t limit, bool histogram) {
    qtsvfs::FileReader fr;
    qtsvfs::KdbFile db;
    std::string err;
    if (!openKdb(p, fr, db, err)) {
        std::fprintf(stderr, "错误: %s (%s)\n", err.c_str(), wideToUtf8(p).c_str());
        return 2;
    }
    std::unordered_map<std::uint64_t, std::uint64_t> sizes;
    std::uint64_t shown = 0;
    std::uint64_t total = db.forEachRecord(
        [&](const qtsvfs::KdbRecord& r) {
            const std::uint64_t tag =
                (static_cast<std::uint64_t>(r.keyLen) << 32) | r.valueLen;
            ++sizes[tag];
            // Qtsf 流第一个字段是 [u32 长度][字符串]，包头串为 "QTSF_PACKAGE"。
            bool head = r.value.size() >= 16 && le32At(r.value, 0) == 12 &&
                        std::memcmp(r.value.data() + 4, "QTSF_PACKAGE", 12) == 0;
            if (!histogram && shown < limit) {
                std::printf("@0x%08X h1=%s h2=%s keylen=%u vallen=%u%s  key=%s\n", r.offset,
                            toHexU32(r.hash1).c_str(), toHexU32(r.hash2).c_str(), r.keyLen,
                            r.valueLen, head ? "  [QTSF_PACKAGE]" : "",
                            qtsvfs::hexdump(r.key.data(), std::min<std::size_t>(r.key.size(), 16), 0)
                                .c_str());
                ++shown;
            }
            return true;
        },
        err);
    if (!err.empty()) {
        std::fprintf(stderr, "遍历中断: %s\n", err.c_str());
    }
    std::printf("\n记录总数 %llu，(keyLen,valueLen) 直方图:\n", static_cast<unsigned long long>(total));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> rows(sizes.begin(), sizes.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < rows.size() && i < 12; ++i) {
        std::printf("  key=%-5u value=%-8u x %llu\n", static_cast<std::uint32_t>(rows[i].first >> 32),
                    static_cast<std::uint32_t>(rows[i].first & 0xFFFFFFFFu),
                    static_cast<unsigned long long>(rows[i].second));
    }
    return err.empty() ? 0 : 3;
}

// raw: 按 key（u64 十六进制）或记录偏移取出元数据卷里的原始记录值，用于读特殊流节点。
int cmdRaw(const std::filesystem::path& p, const std::string& keyHex, std::uint64_t off,
           bool hasOff, const std::string& outPath, std::uint64_t headBytes) {
    qtsvfs::FileReader fr;
    qtsvfs::KdbFile db;
    std::string err;
    if (!openKdb(p, fr, db, err)) {
        std::fprintf(stderr, "错误: %s\n", err.c_str());
        return 2;
    }
    const std::uint64_t want = keyHex.empty() ? 0 : std::strtoull(keyHex.c_str(), nullptr, 16);
    int found = 0;
    db.forEachRecord(
        [&](const qtsvfs::KdbRecord& r) {
            const std::uint64_t k =
                r.key.size() >= 8 ? le32At(r.key, 0) | (static_cast<std::uint64_t>(le32At(r.key, 4)) << 32) : 0;
            const bool match = (!keyHex.empty() && k == want) || (hasOff && r.offset == off);
            if (!match) {
                return true;
            }
            ++found;
            std::printf("@0x%08X key=%016llX h2=%s keylen=%u vallen=%u", r.offset,
                        static_cast<unsigned long long>(k), toHexU32(r.hash2).c_str(), r.keyLen,
                        r.valueLen);
            if (r.key.size() >= 16) {
                std::uint32_t blk = 0, pg = 0;
                std::memcpy(&blk, r.key.data() + 8, 4);
                std::memcpy(&pg, r.key.data() + 12, 4);
                std::printf("  block=%u page=%u", blk, pg);
            }
            std::printf("\n");
            std::fputs(qtsvfs::hexdump(r.value.data(), std::min<std::size_t>(r.value.size(), headBytes), 0)
                           .c_str(),
                       stdout);
            if (!outPath.empty()) {
                std::ofstream os(outPath, std::ios::binary | std::ios::trunc);
                os.write(reinterpret_cast<const char*>(r.value.data()),
                         static_cast<std::streamsize>(r.value.size()));
                std::printf("  -> 写出 %zu 字节: %s\n", r.value.size(), outPath.c_str());
            }
            return true;
        },
        err);
    if (!found) {
        std::fprintf(stderr, "未找到匹配记录\n");
        return 3;
    }
    return 0;
}

int cmdNodes(const std::filesystem::path& p, std::uint64_t limit, bool fields = false) {
    qtsvfs::FileReader fr;
    qtsvfs::KdbFile db;
    std::string err;
    if (!openKdb(p, fr, db, err)) {
        std::fprintf(stderr, "错误: %s\n", err.c_str());
        return 2;
    }
    std::uint64_t shown = 0, parsed = 0, other = 0;
    db.forEachRecord(
        [&](const qtsvfs::KdbRecord& r) {
            qtsvfs::FileNode node;
            if (!node.parse(r.value.data(), r.value.size())) {
                ++other;
                return true;
            }
            ++parsed;
            if (fields) {
                // 全字段 TSV：查「已定名 vs 未定名」是否在某个头字段上系统性不同
                std::printf("%016llX\t%llu\t%u\t%u\t%llu\t%llu\t%u",
                            static_cast<unsigned long long>(node.hash),
                            static_cast<unsigned long long>(node.size), node.obsolete, node.version,
                            static_cast<unsigned long long>(node.checkA),
                            static_cast<unsigned long long>(node.checkB), node.blockCount);
                for (const auto& b : node.blocks) {
                    std::printf("\t0x%X:0x%08X:0x%X", b.startPos, b.packed, b.blockSize);
                }
                std::printf("\n");
                ++shown;
                return true;
            }
            if (shown < limit) {
                std::printf("%016llX size=%-10llu ver=0x%08X blocks=%u", static_cast<unsigned long long>(node.hash),
                            static_cast<unsigned long long>(node.size), node.version, node.blockCount);
                for (const auto& b : node.blocks) {
                    std::printf(" {start=0x%X packed=0x%08X blk=0x%X}", b.startPos, b.packed, b.blockSize);
                }
                std::printf("\n");
                ++shown;
            }
            return true;
        },
        err);
    std::printf("\nFileNode 解析成功 %llu，非节点记录 %llu%s\n", static_cast<unsigned long long>(parsed),
                static_cast<unsigned long long>(other),
                (err.empty() ? std::string() : "  遍历警告: " + err).c_str());
    return 0;
}

static const char* kIndexLiterals[] = {"/(qts-exportsetting-node-index).data",
                                                "/(qts-remaindir-node-index).data"};

void printNode(const qtsvfs::QtsfNode& node, int depth, std::uint64_t& files,
               std::uint64_t& dirs, std::uint64_t limit) {
    for (int i = 0; i < depth; ++i) {
        std::printf("  ");
    }
    std::printf("%s%s", node.name.empty() ? "/" : node.name.c_str(), node.dir ? "/" : "");
    if (!node.dir) {
        std::printf("  hash=%016llX", static_cast<unsigned long long>(node.hash));
        ++files;
    } else {
        std::printf("  dir(%zu)", node.children.size());
        ++dirs;
    }
    std::printf("\n");
    if (node.dir && static_cast<std::uint64_t>(depth) < limit) {
        for (const auto& c : node.children) {
            printNode(c, depth + 1, files, dirs, limit);
        }
    }
}

int cmdTree(const std::filesystem::path& pkgDir, std::uint64_t depthLimit,
            const std::string& outNames) {
    qtsvfs::Package pkg;
    std::string err;
    if (!pkg.open(pkgDir, err)) {
        std::fprintf(stderr, "打开包失败: %s\n", err.c_str());
        return 2;
    }
    if (!pkg.loadNodes(err)) {
        std::fprintf(stderr, "没有解析出 FileNode: %s\n", err.c_str());
        return 2;
    }
    std::printf("包 %s：FileNode %zu 个，数据卷 %zu 个\n",
                wideToUtf8(pkgDir.filename().native()).c_str(), pkg.nodes().size(),
                pkg.volumeCount());
    for (const char* lit : kIndexLiterals) {
        const std::uint64_t hash = qtsvfs::calcHashCode64(lit);
        const auto it = pkg.nodes().find(hash);
        if (it == pkg.nodes().end()) {
            std::printf("\n%s = %016llX：本包无此节点\n", lit, static_cast<unsigned long long>(hash));
            continue;
        }
        std::vector<std::uint8_t> blob;
        if (!pkg.readBlob(it->second, blob, err)) {
            std::printf("\n%s：取块失败 %s\n", lit, err.c_str());
            continue;
        }
        qtsvfs::QtsfNode root;
        if (!qtsvfs::parseQtsfNode(blob.data(), blob.size(), root)) {
            std::printf("\n%s：解出 %zu 字节，但节点流解析失败（首 16 字节 %s）\n", lit, blob.size(),
                        bytesToHex(blob.data(), std::min<std::size_t>(16, blob.size())).c_str());
            continue;
        }
        std::uint64_t files = 0, dirs = 0;
        std::printf("\n%s：未压缩 %llu 字节，解出 %zu 字节\n", lit,
                    static_cast<unsigned long long>(it->second.size), blob.size());
        printNode(root, 0, files, dirs, depthLimit);
        std::printf("  → 目录 %llu，文件 %llu\n", static_cast<unsigned long long>(dirs),
                    static_cast<unsigned long long>(files));
        if (!outNames.empty()) {
            std::vector<std::pair<std::uint64_t, std::string>> table;
            table.reserve(files + 1);
            qtsvfs::collectHashPaths(root, "", table);
            std::ofstream os(outNames, std::ios::binary | std::ios::trunc);
            for (const auto& [h, path] : table) {
                os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h
                   << '\t' << path << '\n';
            }
            std::printf("  → 已写出 hash→path 共 %zu 行: %s\n", table.size(), outNames.c_str());
        }
    }
    return 0;
}

// 在解压后的明文里找自声明路径标记，并用节点哈希自校验。
void reportSelfDeclared(const std::vector<std::uint8_t>& blob, std::uint64_t nodeHash,
                        const char* tag) {
    static const std::regex marker(
        "([A-Za-z]*Path:)([ -~]{4,200}?(\\.mjs|\\.lua|\\.json|\\.txt|\\.js))");
    const std::string text(reinterpret_cast<const char*>(blob.data()), blob.size());
    std::smatch m;
    auto head = text.begin();
    int shown = 0;
    while (std::regex_search(head, text.end(), m, marker) && shown < 6) {
        const std::string path = m[2].str();
        const std::uint64_t h = qtsvfs::calcHashCode64(path);
        const bool ok = h == nodeHash || qtsvfs::calcHashCode64("/" + path) == nodeHash;
        std::printf("  %s 自声明: %s%s -> %s\n", tag, m[1].str().c_str(), path.c_str(),
                    ok ? "哈希自校验通过" : "不匹配");
        ++shown;
        head = m.suffix().first;
    }
    if (shown == 0) {
        std::printf("  %s 未发现自声明标记\n", tag);
    }
}

int cmdExtract(const std::filesystem::path& pkgDir, const std::string& hashHex,
               const std::string& outPath) {
    qtsvfs::Package pkg;
    std::string err;
    if (!pkg.open(pkgDir, err)) {
        std::fprintf(stderr, "打开包失败: %s\n", err.c_str());
        return 2;
    }
    if (!pkg.loadNodes(err)) {
        std::fprintf(stderr, "无 FileNode: %s\n", err.c_str());
        return 2;
    }
    const std::uint64_t hash = std::strtoull(hashHex.c_str(), nullptr, 16);
    const auto it = pkg.nodes().find(hash);
    if (it == pkg.nodes().end()) {
        std::fprintf(stderr, "包内无节点 %s\n", hashHex.c_str());
        return 3;
    }
    const qtsvfs::FileNode& node = it->second;
    std::printf("节点 %s 未压缩=%llu 块数=%u method=%s\n", hashHex.c_str(),
                static_cast<unsigned long long>(node.size), node.blockCount,
                node.blocks.empty() ? "-" : qtsvfs::methodName(static_cast<std::uint8_t>(node.blocks[0].packed & 0xFF)));
    std::vector<std::uint8_t> blob;
    if (!pkg.readBlob(node, blob, err)) {
        std::fprintf(stderr, "读取失败: %s\n", err.c_str());
        return 4;
    }
    std::printf("  解出 %zu 字节", blob.size());
    if (!outPath.empty()) {
        std::ofstream os(outPath, std::ios::binary);
        os.write(reinterpret_cast<const char*>(blob.data()),
                 static_cast<std::streamsize>(blob.size()));
        std::printf(" -> %s", outPath.c_str());
    }
    std::printf("\n");
    reportSelfDeclared(blob, hash, "  ");
    return 0;
}

// ── 实时重建名字表 ──────────────────────────────────────────────────────────────
// 名字表不再是外部文件：把给的根展开成包列表（packages/<id>、MiniApp_<id>/0、
// APK builtin/<id> 三种布局都认），建两道全库索引，再逐包解块取四层来源。
// 展开与建索引的逻辑在 qtsvfs::openLibrary，界面走的是同一个函数，两边不会漂。

// 通用标记键：1..24 个「词字符」+ 词干（如 Path）+ : ，只在 --markers 探索模式里用，
// 用来看明文里还有哪些没被四层来源覆盖的自声明格式。
bool markerKeyAt(const std::string& text, std::size_t pos, std::size_t& keyStart,
                 std::size_t& keyEnd) {
    std::size_t i = 0;
    while (i < pos && i < 24) {
        const char c = text[pos - i - 1];
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '_' || c == '-' || c == '.';
        if (!keep) {
            break;
        }
        ++i;
    }
    if (i < 1) {
        return false;
    }
    keyStart = pos - i;
    keyEnd = pos;
    return true;
}

qtsvfs::Library openImport(const std::vector<std::filesystem::path>& roots, int threads,
                           bool withKeyset, bool withDirs, const std::string& prefix = {}) {
    std::function<bool(const qtsvfs::PackageRef&)> filter;
    if (!prefix.empty()) {
        filter = [&](const qtsvfs::PackageRef& r) {
            // 检查包目录名或其父目录名是否匹配前缀（小程序的包目录是 MiniApp_XXX/0）
            const std::string name = r.dir.filename().string();
            if (name.rfind(prefix, 0) == 0) return true;
            const std::string parent = r.dir.parent_path().filename().string();
            return parent.rfind(prefix, 0) == 0;
        };
    }
    qtsvfs::Library lib = qtsvfs::openLibrary(
        roots, threads, withKeyset, withDirs,
        [](const std::string& stage, std::size_t done, std::size_t total) {
            if (done % 400 == 0 || done == total) {
                std::printf("  %s %zu/%zu", stage.c_str(), done, total);
                std::fflush(stdout);
            }
        },
        {}, filter);
    std::map<std::string, std::size_t> perMount;
    for (const auto& ref : lib.pkgs) {
        ++perMount[ref.mount];
    }
    std::printf("%zu 个包：", lib.pkgs.size());
    for (const auto& [mount, n] : perMount) {
        std::printf(" %s x%zu", mount.c_str(), n);
    }
    if (!lib.keys.empty()) {
        std::printf("；闸门 %zu 个全库节点键", lib.keys.size());
    }
    if (!lib.dirs.empty()) {
        std::printf("；目录段索引 %zu 条", lib.dirs.size());
    }
    std::printf("\n");
    std::fflush(stdout);
    return lib;
}

struct ScanRequest {
    std::uint64_t maxNodes = 0;      // 每包采样上限，0 = 全部
    std::uint64_t maxNodeSize = 0;   // 0 = 用 HarvestOptions 的默认（64M）
    std::string outTsv, missingTsv;
    bool markers = false;  // 探索模式：统计明文里还有哪些「键: 路径」自声明
    bool buckets = false;  // 未定名原因分桶（每个节点多跑一遍字段扫描）
    std::vector<std::string> prefixes;
    const qtsvfs::DirIndex* dirs = nullptr;  // 全库目录段索引，cmdScan 里从 Library 填
    int threads = 0;
};

struct ScanTotal {
    std::uint64_t pkgs = 0, nodes = 0, decoded = 0, failed = 0, oversized = 0, noData = 0;
    std::uint64_t live = 0, liveNamed = 0;
    std::uint64_t treeRows = 0, realRows = 0, objectRows = 0, namedRows = 0;
    std::unordered_set<std::uint64_t> namedKeys;  // 唯一键口径：同键在多个包里只算一次
    std::unordered_map<std::string, std::uint64_t> kinds, failReason, keyFreq;
    std::unordered_map<std::string, std::string> keyExample;
};

// 一个包：开卷 → 收割候选行 → 合并成名单（借全库目录段）→ 报数。
// 解块与合并都在锁外，只有汇总和写盘过锁。
void scanOnePackage(const qtsvfs::PackageRef& ref, const qtsvfs::KeySet* keys,
                    const ScanRequest& req, std::mutex& mx, std::ofstream& rowsOs,
                    std::ofstream& missingOs, ScanTotal& total, std::size_t seq, std::size_t nPkgs) {
    qtsvfs::Package pkg;
    std::string err;
    if (!pkg.open(ref.dir, err) || !pkg.loadNodes(err)) {
        std::lock_guard<std::mutex> lk(mx);
        ++total.pkgs;
        std::printf("  [%zu/%zu] %-22s 打不开: %s\n", seq, nPkgs, ref.label.c_str(),
                    err.empty() ? "包内没有可解析的 FileNode" : err.c_str());
        return;
    }
    qtsvfs::HarvestOptions ho;
    ho.keyset = keys;
    ho.prefixes = req.prefixes;
    ho.maxNodes = req.maxNodes;
    if (req.maxNodeSize) {
        ho.maxNodeSize = req.maxNodeSize;
    }

    // 探索模式的标记直方图：手写 token 扫描，regex 的嵌套量词在长文本上会回溯爆栈。
    std::unordered_map<std::string, std::uint64_t> localKeys;
    std::unordered_map<std::string, std::string> localExample;
    std::function<void(const qtsvfs::FileNode&, const std::vector<std::uint8_t>&)> onBlob;
    if (req.markers) {
        static const auto isPathChar = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-' || c == '.' || c == '/' || c == '+';
        };
        onBlob = [&](const qtsvfs::FileNode& node, const std::vector<std::uint8_t>& blob) {
            (void)node;
            const std::string text(reinterpret_cast<const char*>(blob.data()),
                                   std::min<std::size_t>(blob.size(), 4u << 20));
            std::unordered_set<std::string> seen;
            for (std::size_t sep = text.find(':'); sep != std::string::npos;
                 sep = text.find(':', sep + 1)) {
                std::size_t ks = 0, ke = 0;
                if (!markerKeyAt(text, sep, ks, ke)) {
                    continue;
                }
                const std::size_t s = sep + 1;
                std::size_t e = s;
                while (e < text.size() && isPathChar(text[e]) && e - s < 300) {
                    ++e;
                }
                const std::string tok = text.substr(s, e - s);
                if (tok.size() < 6 || tok.find('/') == std::string::npos ||
                    !std::isalnum(static_cast<unsigned char>(tok.back()))) {
                    continue;
                }
                const std::string key = text.substr(ks, ke - ks) + ":";
                if (!seen.insert(key + "\x01" + tok).second) {
                    continue;
                }
                ++localKeys[key];
                localExample.try_emplace(key, tok);
            }
        };
    }

    std::vector<qtsvfs::NameRow> rows;
    qtsvfs::HarvestStats hs;
    std::unordered_map<std::uint64_t, qtsvfs::CatalogStats> bodyKinds;
    qtsvfs::harvestPackage(pkg, ho, rows, hs, onBlob, req.buckets ? &bodyKinds : nullptr);
    qtsvfs::NameTable names;
    qtsvfs::SourceTable sources;
    qtsvfs::mergeRows(rows, names, &sources, req.dirs);

    std::uint64_t live = 0, liveNamed = 0;
    for (const auto& [h, node] : pkg.nodes()) {
        if (node.obsolete == 0) {
            ++live;
            if (names.count(h)) {
                ++liveNamed;
            }
        }
    }
    std::unordered_map<std::string, std::uint64_t> kinds;
    if (req.buckets) {
        qtsvfs::bucketUnnamed(bodyKinds, names, kinds);
    }

    std::lock_guard<std::mutex> lk(mx);
    ++total.pkgs;
    total.nodes += hs.nodes;
    total.decoded += hs.decoded;
    total.failed += hs.failed;
    total.oversized += hs.oversized;
    total.noData += hs.noData;
    total.live += live;
    total.liveNamed += liveNamed;
    total.treeRows += hs.treeRows;
    total.realRows += hs.realRows;
    total.objectRows += hs.objectRows;
    total.namedRows += hs.namedRows;
    for (const auto& [h, path] : names) {
        (void)path;
        total.namedKeys.insert(h);
    }
    for (const auto& [k, v] : kinds) {
        total.kinds[k] += v;
    }
    for (const auto& [k, v] : hs.failReason) {
        total.failReason[k] += v;
    }
    for (const auto& [k, v] : localKeys) {
        total.keyFreq[k] += v;
        total.keyExample.try_emplace(k, localExample.count(k) ? localExample[k] : std::string());
    }
    if (rowsOs) {
        for (const auto& r : rows) {
            rowsOs << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << r.hash
                   << '\t' << r.tag << '\t' << r.sub << '\t' << r.path << '\n'
                   << std::dec;
        }
        rowsOs.flush();
    }
    if (missingOs) {
        for (const auto& [h, size] : hs.noDataNodes) {
            missingOs << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h
                      << '\t' << std::dec << size << '\t' << ref.label << '\n';
        }
        missingOs.flush();
    }
    const double rate =
        hs.nodes ? 100.0 * static_cast<double>(names.size()) / static_cast<double>(hs.nodes) : 0.0;
    const double liveRate =
        live ? 100.0 * static_cast<double>(liveNamed) / static_cast<double>(live) : 0.0;
    std::printf("  [%zu/%zu] %-22s 节点 %8llu 解出 %8llu 定名 %8llu(%4.1f%%) 活节点 %4.1f%% "
                "行 t/r/o/n %llu/%llu/%llu/%llu 无块 %llu 解败 %llu\n",
                seq, nPkgs, ref.label.c_str(),
                static_cast<unsigned long long>(hs.nodes),
                static_cast<unsigned long long>(hs.decoded),
                static_cast<unsigned long long>(names.size()), rate, liveRate,
                static_cast<unsigned long long>(hs.treeRows),
                static_cast<unsigned long long>(hs.realRows),
                static_cast<unsigned long long>(hs.objectRows),
                static_cast<unsigned long long>(hs.namedRows),
                static_cast<unsigned long long>(hs.noData),
                static_cast<unsigned long long>(hs.failed));
    std::fflush(stdout);
}

int cmdScan(qtsvfs::Library& lib, ScanRequest req) {
    req.dirs = lib.dirs.empty() ? nullptr : &lib.dirs;
    std::ofstream rowsOs, missingOs;
    if (!req.outTsv.empty()) {
        rowsOs.open(utf8ToPath(req.outTsv), std::ios::binary | std::ios::trunc);
        if (!rowsOs) {
            std::fprintf(stderr, "写不出 %s\n", req.outTsv.c_str());
            return 2;
        }
    }
    if (!req.missingTsv.empty()) {
        missingOs.open(utf8ToPath(req.missingTsv), std::ios::binary | std::ios::trunc);
        if (!missingOs) {
            std::fprintf(stderr, "写不出 %s\n", req.missingTsv.c_str());
            return 2;
        }
    }
    ScanTotal total;
    std::mutex mx;
    std::atomic<std::size_t> next{0};
    const std::size_t n = lib.pkgs.size();
    auto worker = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= n) {
                break;
            }
            scanOnePackage(lib.pkgs[i], lib.keys.empty() ? nullptr : &lib.keys, req, mx, rowsOs,
                           missingOs, total, i + 1, n);
        }
    };
    int threads = req.threads > 0 ? req.threads
                                  : static_cast<int>(std::thread::hardware_concurrency());
    threads = (std::max)(1, (std::min)(threads, 32));
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>((std::max)(1, threads - 1)));
    for (int i = 1; i < threads; ++i) {
        pool.emplace_back(worker);
    }
    worker();
    for (auto& t : pool) {
        t.join();
    }
    const double secs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count() / 1000.0;

    // 三个分母一起报：全节点、扣掉盘上无字节的、只算原厂还认的活节点。
    // 少了任何一个，听起来都像还差 30% 的功力，而实际上那是数据的边界。
    const std::uint64_t decodable = total.nodes > total.noData ? total.nodes - total.noData : 0;
    const double rateAll =
        total.nodes ? 100.0 * static_cast<double>(total.namedKeys.size()) / static_cast<double>(total.nodes) : 0.0;
    const double rateDecoded =
        decodable ? 100.0 * static_cast<double>(total.namedKeys.size()) / static_cast<double>(decodable) : 0.0;
    const double liveRate =
        total.live ? 100.0 * static_cast<double>(total.liveNamed) / static_cast<double>(total.live) : 0.0;
    std::printf("\n实时重建完成：%zu 个包 %.1f 秒（%d 线程）。唯一键 %llu 个已定名 = %.1f%%；"
                "节点 %llu 个（扣盘上无字节 %llu）定名 = %.1f%%；"
                "活节点（FileNode 废弃位=0）%llu 里定名 %llu = %.1f%%\n",
                total.pkgs, secs, threads,
                static_cast<unsigned long long>(total.namedKeys.size()), rateAll,
                static_cast<unsigned long long>(total.nodes),
                static_cast<unsigned long long>(total.noData), rateDecoded,
                static_cast<unsigned long long>(total.live),
                static_cast<unsigned long long>(total.liveNamed), liveRate);
    std::printf("  候选行 tree %llu / real %llu / object %llu / named %llu；"
                "解出 %llu 解压失败 %llu 超限跳过 %llu 盘上无块 %llu\n",
                static_cast<unsigned long long>(total.treeRows),
                static_cast<unsigned long long>(total.realRows),
                static_cast<unsigned long long>(total.objectRows),
                static_cast<unsigned long long>(total.namedRows),
                static_cast<unsigned long long>(total.decoded),
                static_cast<unsigned long long>(total.failed),
                static_cast<unsigned long long>(total.oversized),
                static_cast<unsigned long long>(total.noData));
    if (!lib.keys.empty()) {
        std::printf("  全库唯一节点键 %zu 个，其中未定名 %llu 个\n", lib.keys.size(),
                    lib.keys.size() > total.namedKeys.size()
                        ? static_cast<unsigned long long>(lib.keys.size() - total.namedKeys.size())
                        : 0ull);
    } else {
        std::printf("  没建全库键集合：闸门只用各包自己的节点表，跨包引用定不出名\n");
    }
    if (!total.failReason.empty()) {
        std::vector<std::pair<std::string, std::uint64_t>> fails(total.failReason.begin(),
                                                                total.failReason.end());
        std::sort(fails.begin(), fails.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("  解压失败原因 Top6:\n");
        for (std::size_t i = 0; i < fails.size() && i < 6; ++i) {
            std::printf("    x%-9llu %s\n", static_cast<unsigned long long>(fails[i].second),
                        fails[i].first.c_str());
        }
    }
    if (!total.kinds.empty()) {
        std::vector<std::pair<std::string, std::uint64_t>> kinds(total.kinds.begin(),
                                                                total.kinds.end());
        std::sort(kinds.begin(), kinds.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::uint64_t sum = 0;
        for (const auto& kv : kinds) {
            sum += kv.second;
        }
        std::printf("  有字节但未定名 %llu 个，按原因分桶:\n", static_cast<unsigned long long>(sum));
        for (std::size_t i = 0; i < kinds.size() && i < 12; ++i) {
            std::printf("    %-40s %llu\n", kinds[i].first.c_str(),
                        static_cast<unsigned long long>(kinds[i].second));
        }
    }
    if (req.markers) {
        std::vector<std::pair<std::string, std::uint64_t>> keys(total.keyFreq.begin(),
                                                               total.keyFreq.end());
        std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) {
                return a.second > b.second;
            }
            return a.first < b.first;
        });
        std::printf("  明文里的「键: 路径」自声明 Top20（次数 | 例子）:\n");
        for (std::size_t i = 0; i < keys.size() && i < 20; ++i) {
            const auto ex = total.keyExample.find(keys[i].first);
            std::printf("    %-24s %-9llu %s\n", keys[i].first.c_str(),
                        static_cast<unsigned long long>(keys[i].second),
                        ex == total.keyExample.end() ? "" : ex->second.substr(0, 72).c_str());
        }
    }
    if (rowsOs) {
        std::printf("  → 候选行清单: %s\n", req.outTsv.c_str());
    }
    if (missingOs) {
        std::printf("  → 盘上无字节节点: %s\n", req.missingTsv.c_str());
    }
    return 0;
}

// 打印一个包的目录树。名单与树都在 qtsvfs::openPackageEntry 里现算，
// 和界面点开包时走的是同一个函数。
int cmdLayout(qtsvfs::Library& lib, const std::string& packageArg,
              const std::filesystem::path& preferDir, std::uint64_t depthLimit,
              const std::string& outTsv) {
    if (lib.pkgs.empty()) {
        std::fprintf(stderr, "没有可用的包目录\n");
        return 2;
    }
    const qtsvfs::PackageRef* target = &lib.pkgs[0];
    if (!packageArg.empty()) {
        const auto it = std::find_if(lib.pkgs.begin(), lib.pkgs.end(),
                                     [&](const qtsvfs::PackageRef& r) {
                                         return r.label == packageArg ||
                                                r.dir.filename().string() == packageArg ||
                                                r.mount == packageArg;
                                     });
        if (it == lib.pkgs.end()) {
            std::fprintf(stderr, "展开的包里没有 %s（给包号 101、小程序 MiniApp_10617/0 或 mount 名）\n",
                         packageArg.c_str());
            return 2;
        }
        target = &*it;
    } else if (!preferDir.empty()) {
        // 传的就是一个包目录时看它自己 —— 闸门仍然用整个实例的全库键。
        const auto it = std::find_if(lib.pkgs.begin(), lib.pkgs.end(),
                                     [&](const qtsvfs::PackageRef& r) { return r.dir == preferDir; });
        if (it != lib.pkgs.end()) {
            target = &*it;
        }
    }
    const std::shared_ptr<qtsvfs::PackageEntry> entry = qtsvfs::openPackageEntry(lib, *target);
    if (!entry) {
        std::fprintf(stderr, "打开包或读不出 FileNode: %s\n", target->label.c_str());
        return 2;
    }
    const qtsvfs::TreeNode& root = entry->root;
    const qtsvfs::TreeStats& st = entry->treeStats;
    std::printf("包 %s（闸门 %zu 个键，目录段索引 %zu 条）：节点 %zu，真名 %llu，未定名 %llu，"
                "未压总字节 %llu\n",
                target->label.c_str(), lib.keys.size(), lib.dirs.size(), entry->nodes.size(),
                static_cast<unsigned long long>(st.named),
                static_cast<unsigned long long>(st.nameless),
                static_cast<unsigned long long>(st.bytes));
    std::printf("  未定名里 %llu 个带原厂废弃标记（收进 [obsolete]）\n",
                static_cast<unsigned long long>(st.obsolete));
    qtsvfs::TreeStats walked;
    qtsvfs::printTree(root, stdout, 0, depthLimit, walked);
    std::printf("  → 打印目录 %llu，文件 %llu（含未定名 %llu）\n",
                static_cast<unsigned long long>(walked.dirs),
                static_cast<unsigned long long>(walked.files),
                static_cast<unsigned long long>(walked.nameless));
    if (!outTsv.empty()) {
        std::vector<qtsvfs::TreeRow> rows;
        qtsvfs::collectRows(root, rows, 0, depthLimit);
        std::ofstream os(utf8ToPath(outTsv), std::ios::binary | std::ios::trunc);
        os << "path\thash\tsize\tmethod\tsource\n";
        for (const auto& r : rows) {
            char hex[24];
            std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(r.hash));
            os << r.path << '\t' << hex << '\t' << r.size << '\t' << unsigned(r.method) << '\t'
               << (r.source.empty() ? (r.named ? "named" : "nameless") : r.source) << '\n';
        }
        std::printf("  → 文件树清单 %zu 行: %s\n", rows.size(), outTsv.c_str());
    }
    return 0;
}
int cmdExport(qtsvfs::Library& lib, const std::vector<qtsvfs::PackageRef>& targets,
              const std::string& outDir, std::uint64_t limit, bool nameless, bool skipObsolete,
              const std::string& manifest, const std::string& state, int threads,
              bool normalize, const std::vector<std::string>& prefixes) {
    if (targets.empty()) {
        std::fprintf(stderr, "没有要导出的包\n");
        return 2;
    }
    qtsvfs::ExportOptions opt;
    opt.allowNameless = nameless;
    opt.skipObsolete = skipObsolete;
    opt.limit = limit;
    opt.threads = threads;
    opt.normalizeSerialized = normalize;
    opt.keyset = lib.keys.empty() ? nullptr : &lib.keys;
    opt.dirs = lib.dirs.empty() ? nullptr : &lib.dirs;
    opt.prefixes = prefixes;
    if (!manifest.empty()) {
        opt.manifestPath = utf8ToPath(manifest);
    }
    if (!state.empty()) {
        opt.statePath = utf8ToPath(state);
    }
    std::size_t mounts = 0;
    {
        std::unordered_set<std::string> seen;
        for (const auto& ref : targets) {
            if (seen.insert(ref.mount).second) {
                ++mounts;
            }
        }
    }
    std::printf("%zu 个包（%zu 个 mount，并行度 %d，闸门 %zu 键），名单逐包现算，导出到 %s"
                "（未定名%s，废弃节点%s，归一化%s，manifest=%s，state=%s）\n",
                targets.size(), mounts, threads, lib.keys.size(), outDir.c_str(),
                nameless ? "带哈希名落盘" : "跳过",
                skipObsolete ? "跳过" : "照导", normalize ? "开" : "关",
                manifest.empty() ? "无" : manifest.c_str(),
                state.empty() ? "无（不续跑）" : state.c_str());
    std::fflush(stdout);
    std::atomic<int> seen{0};
    const qtsvfs::ExportSummary r = qtsvfs::exportAll(
        targets, utf8ToPath(outDir), opt,
        [&](const qtsvfs::ExportSummary& cur, const std::string& pkgName) {
            std::printf("  [%d/%llu] %-24s 文件 %llu / %llu MB  定名 %llu/%llu  "
                        "无名 %llu 废弃 %llu 无块 %llu 解败 %llu 截断 %llu\n",
                        ++seen, static_cast<unsigned long long>(cur.pending), pkgName.c_str(),
                        static_cast<unsigned long long>(cur.files),
                        static_cast<unsigned long long>(cur.bytes >> 20),
                        static_cast<unsigned long long>(cur.namedNodes),
                        static_cast<unsigned long long>(cur.totalNodes),
                        static_cast<unsigned long long>(cur.noName),
                        static_cast<unsigned long long>(cur.obsolete),
                        static_cast<unsigned long long>(cur.noData),
                        static_cast<unsigned long long>(cur.decodeFailed),
                        static_cast<unsigned long long>(cur.shortBlob));
            std::fflush(stdout);
        });
    std::printf(
        "  → 导出 %llu 个文件共 %llu 字节（归一化 SerializedFile %llu 个）；实时名单定名 %llu/%llu 节点；"
        "跳过：未定名 %llu、废弃 %llu、续跑已有 %llu 个包；异常：盘上无块 %llu、解压失败 %llu、"
        "长度不符 %llu、写盘失败 %llu、改名 %llu、包打不开 %llu%s\n",
        static_cast<unsigned long long>(r.files), static_cast<unsigned long long>(r.bytes),
        static_cast<unsigned long long>(r.normalized),
        static_cast<unsigned long long>(r.namedNodes),
        static_cast<unsigned long long>(r.totalNodes),
        static_cast<unsigned long long>(r.noName), static_cast<unsigned long long>(r.obsolete),
        static_cast<unsigned long long>(r.resumed), static_cast<unsigned long long>(r.noData),
        static_cast<unsigned long long>(r.decodeFailed),
        static_cast<unsigned long long>(r.shortBlob),
        static_cast<unsigned long long>(r.writeFailed),
        static_cast<unsigned long long>(r.renamed),
        static_cast<unsigned long long>(r.dirsFailed), r.cancelled ? "  [已取消]" : "");
    if (!r.error.empty()) {
        std::printf("  最后一条错误: %s\n", r.error.c_str());
    }
    return r.error.empty() || r.files > 0 ? 0 : 3;
}


// SerializedFile 里 m_StreamData.path 写的是流节点的规范路径（assets/xx/<哈希>.resS），
// 名字表给这些节点的多半是 catalog 显示名，两边对不上，AssetRipper 就找不到纹理像素。
// 这里扫导出树把引用收齐，按「小写路径哈希 == 节点键」对上 manifest，给已导出的文件
// 补一个规范名的硬链接（不占空间），下游按引用名就能直接打开。
int cmdAlias(const std::string& exportRoot, const std::string& manifest, bool dryRun, int threads,
             const std::string& refs) {
    const qtsvfs::AliasResult r = qtsvfs::linkStreamAliases(
        utf8ToPath(exportRoot), utf8ToPath(manifest), dryRun, threads,
        refs.empty() ? std::filesystem::path{} : utf8ToPath(refs),
        [](const qtsvfs::AliasResult& cur) {
            std::printf("  … 已扫 %llu/%llu 个文件，引用 %llu，命中 %llu，已接 %llu\n",
                        static_cast<unsigned long long>(cur.scanned),
                        static_cast<unsigned long long>(cur.total),
                        static_cast<unsigned long long>(cur.refs),
                        static_cast<unsigned long long>(cur.hit),
                        static_cast<unsigned long long>(cur.linked));
            std::fflush(stdout);
        });
    std::printf("  → 扫 %llu 个文件：流引用 %llu 条，哈希命中 %llu，补硬链接 %llu，"
                "本来就在 %llu，目标不在盘上 %llu，失败 %llu%s\n",
                static_cast<unsigned long long>(r.scanned), static_cast<unsigned long long>(r.refs),
                static_cast<unsigned long long>(r.hit), static_cast<unsigned long long>(r.linked),
                static_cast<unsigned long long>(r.already), static_cast<unsigned long long>(r.stale),
                static_cast<unsigned long long>(r.failed), dryRun ? "  [dry-run]" : "");
    if (!r.error.empty()) {
        std::printf("  最后一条错误: %s\n", r.error.c_str());
    }
    return r.error.empty() ? 0 : 3;
}

// 全库节点键清单（哈希, 声明长度, 包）：实时重建用不到它（KeySet 在内存里现建），
// 留着是给外部脚本做分母与交叉核对用的。包列表来自 discoverPackages，所以
// packages 与 MiniApp 能在同一次里导全。
int cmdKeys(const std::vector<qtsvfs::PackageRef>& pkgs, const std::string& outTsv) {
    std::ofstream os(utf8ToPath(outTsv), std::ios::binary | std::ios::trunc);
    if (!os) {
        std::fprintf(stderr, "写不出 %s\n", outTsv.c_str());
        return 2;
    }
    std::uint64_t total = 0, bad = 0;
    for (const auto& ref : pkgs) {
        qtsvfs::Package pkg;
        std::string err;
        if (!pkg.open(ref.dir, err) || !pkg.loadNodes(err)) {
            ++bad;
            continue;
        }
        for (const auto& [h, n] : pkg.nodes()) {
            os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h << '\t'
               << std::dec << n.size << '\t' << ref.label << '\n';
        }
        total += pkg.nodes().size();
    }
    std::printf("导出 %llu 个节点键（%zu 个包，跳过 %llu）→ %s\n",
                static_cast<unsigned long long>(total), pkgs.size(),
                static_cast<unsigned long long>(bad), outTsv.c_str());
    return 0;
}

int cmdGindex(const std::filesystem::path& dataFile, const std::string& namesFile) {
    qtsvfs::GlobalIndex gi;
    std::string err;
    if (!gi.load(dataFile, err)) {
        std::fprintf(stderr, "加载失败: %s\n", err.c_str());
        return 2;
    }
    const auto& h = gi.header();
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(h.internalPackageHash));
    std::printf("formatVersion=%u ver=%u buildId=%llu internalPackageHash=0x%s\n", h.formatVersion,
                h.ver, static_cast<unsigned long long>(h.buildId), hex);
    std::printf("numPackages=%u numFiles=%u conflicts=%u method=%u compressed=%u\n", h.numPackages,
                h.numFiles, h.numConflictFiles, h.compressMethod, h.compressedDataSize);
    std::printf("数组区条目=%zu\n", gi.fileCount());
    if (namesFile.empty()) {
        return 0;
    }
    std::ifstream in(namesFile);
    std::uint64_t total = 0, hitLo = 0, hitHi = 0, miss = 0;
    std::unordered_map<std::uint32_t, std::uint64_t> perPackage;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::uint64_t hash = std::strtoull(line.substr(0, tab).c_str(), nullptr, 16);
        ++total;
        bool fromLow = false;
        const std::uint16_t pkg = gi.findPackageByNodeHash(hash, &fromLow);
        if (pkg == 0xFFFF) {
            ++miss;
            continue;
        }
        (fromLow ? hitLo : hitHi)++;
        perPackage[pkg]++;
        if (total <= 5) {
            std::printf("  %s -> 包 %u (取%s32位)\n", line.substr(tab + 1).c_str(), pkg,
                        fromLow ? "低" : "高");
        }
    }
    std::printf("命中 %llu（低32位 %llu / 高32位 %llu），未命中 %llu，共 %llu 条名字\n",
                static_cast<unsigned long long>(hitLo + hitHi),
                static_cast<unsigned long long>(hitLo), static_cast<unsigned long long>(hitHi),
                static_cast<unsigned long long>(miss), static_cast<unsigned long long>(total));
    for (const auto& [pkg, n] : perPackage) {
        std::printf("  包 %u: %llu 个已命名文件\n", pkg, static_cast<unsigned long long>(n));
    }
    return 0;
}

// 离线合并候选行成一张名字表（tree > real > object > named 的优先级、目录段补全、
// 同名消歧全在 qtsvfs::mergeRows 里，和实时重建走的是同一份实现，不会两头漂移）。
// 用法是把手边几份 scan 产出的 TSV 并起来，做覆盖率对账或离线分档时用。
int cmdMergeNames(const std::vector<std::filesystem::path>& ins, const std::string& outTsv) {
    std::vector<qtsvfs::NameRow> rows;
    std::map<std::string, std::uint64_t> bySource;
    for (const auto& f : ins) {
        std::ifstream in(f);
        if (!in) {
            std::fprintf(stderr, "打不开 %s\n", wideToUtf8(f).c_str());
            return 2;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.size() < 18 || line[16] != '\t') {
                continue;
            }
            // 行形如 HASH<TAB>tag<TAB>sub<TAB>path（named 行的 sub 为空）。
            qtsvfs::NameRow r;
            std::size_t t = 17;
            for (int col = 0; col < 3; ++col) {
                const std::size_t nx = col == 2 ? std::string::npos : line.find('\t', t);
                const std::string field =
                    nx == std::string::npos ? line.substr(t) : line.substr(t, nx - t);
                if (col == 0) {
                    r.tag = field;
                } else if (col == 1) {
                    r.sub = field;
                } else {
                    r.path = field;
                }
                if (nx == std::string::npos) {
                    break;
                }
                t = nx + 1;
            }
            if (r.path.empty() || r.tag.empty()) {
                continue;
            }
            if (r.tag != "real" && r.tag != "object" && r.tag != "named" && r.tag != "tree") {
                continue;
            }
            r.hash = std::strtoull(line.substr(0, 16).c_str(), nullptr, 16);
            if (r.hash == 0) {
                continue;
            }
            ++bySource[r.tag + "/" + r.sub];
            rows.push_back(std::move(r));
        }
    }

    qtsvfs::NameTable table;
    qtsvfs::SourceTable sources;
    qtsvfs::mergeRows(rows, table, &sources);
    std::ofstream os(utf8ToPath(outTsv));
    if (!os) {
        std::fprintf(stderr, "写不出 %s\n", outTsv.c_str());
        return 3;
    }
    for (const auto& [h, p] : table) {
        const auto s = sources.find(h);
        const std::string full = s == sources.end() ? std::string("named") : s->second;
        const std::size_t colon = full.find(':');
        const std::string tag = colon == std::string::npos ? full : full.substr(0, colon);
        const std::string sub = colon == std::string::npos ? std::string() : full.substr(colon + 1);
        os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h << '\t' << tag
           << '\t' << sub << '\t' << p << '\n'
           << std::dec;
    }
    std::printf("合并 %zu 个来源文件 %zu 行 -> %s，共 %zu 个节点定名（同键按 tree>real>object>named 只留一条，"
                "被覆盖 %zu 行）\n",
                ins.size(), rows.size(), outTsv.c_str(), table.size(),
                rows.size() > table.size() ? rows.size() - table.size() : 0ull);
    for (const auto& [k, v] : bySource) {
        std::printf("  %-28s %llu\n", k.c_str(), static_cast<unsigned long long>(v));
    }
    return 0;
}

int cmdProbe(const std::filesystem::path& file, std::uint64_t head_bytes, std::uint64_t sample_bytes,
             std::uint64_t block_bytes) {
    qtsvfs::FileReader fr;
    std::string err;
    if (!fr.open(file, err)) {
        std::fprintf(stderr, "错误: %s\n", err.c_str());
        return 2;
    }
    std::printf("file   : %s\n", wideToUtf8(file).c_str());
    std::printf("size   : %llu bytes\n", static_cast<unsigned long long>(fr.size()));

    const std::uint64_t head = std::min<std::uint64_t>(head_bytes, fr.size());
    auto head_buf = fr.read(0, static_cast<std::size_t>(head), err);
    if (!err.empty()) {
        std::fprintf(stderr, "错误: %s\n", err.c_str());
        return 2;
    }
    std::printf("\n-- header %llu bytes --\n", static_cast<unsigned long long>(head));
    std::fputs(qtsvfs::hexdump(head_buf.data(), head_buf.size(), 0).c_str(), stdout);

    auto scan = qtsvfs::scanU32AsOffsets(head_buf.data(), head_buf.size(), 0, fr.size(), 12);
    std::printf("\n-- u32 列按文件内偏移解释: %zu/%zu 落在 [0,size) 且非 0xFFFFFFFF\n", scan.in_range,
                scan.total);
    for (const auto& [off, v] : scan.samples) {
        std::printf("   @%llu -> 0x%08X (%llu)\n", static_cast<unsigned long long>(off), v,
                    static_cast<unsigned long long>(v));
    }

    const std::uint64_t want = std::min<std::uint64_t>(sample_bytes, fr.size());
    const std::uint64_t aligned = want - (want % block_bytes);
    if (aligned >= block_bytes) {
        auto buf = fr.read(0, static_cast<std::size_t>(aligned), err);
        if (!err.empty()) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 2;
        }
        const auto st = qtsvfs::measureEntropy(buf, block_bytes);
        std::printf("\n-- 熵（前 %llu bytes，块 %llu）--\n",
                    static_cast<unsigned long long>(aligned),
                    static_cast<unsigned long long>(block_bytes));
        std::printf("   overall %.4f | block mean %.4f | min %.4f | max %.4f | 高熵块占比 %.1f%%\n",
                    st.overall, st.block_mean, st.block_min, st.block_max,
                    st.high_entropy_ratio * 100.0);
    }
    return 0;
}

void usage() {
    std::printf(
        "qtsvfs — QtsVFS 虚拟文件系统分析/管理工具\n"
        "\n"
        "用法:\n"
        "  名字表不再需要外部文件：layout/scan/export 把包展开后逐包现算名单\n"
        "  （四层来源 tree>real>object>named 在 qtsvfs::harvestPackage 里，界面走同一条路）。\n"
        "  两步预处理：KeySet（全库节点键，只读元数据卷，几秒）给哈希闸门用；\n"
        "  DirIndex（全库目录段索引，要把块解一遍，全库约一两分钟）给体内自声明的名字补目录段，\n"
        "  --no-keyset / --no-dirs 分别关掉它们。\n"
        "  路径参数是「根」，三种布局都认：packages/ 根、MiniApp_<id>/ 根、QtsVFSCache/ 根\n"
        "  （一次把主缓存和 55 个小程序都展开）、以及单个包目录 packages/101。\n"
        "\n"
        "  qtsvfs codecs                          已接入的压缩库\n"
        "  qtsvfs hash <path>...                  计算节点哈希 (VFS_CalcHashCode64Raw)\n"
        "  qtsvfs kdb info <file.db>              外层 QtskDB 头\n"
        "  qtsvfs kdb records <file.db> [--max=N] [--hist]   枚举 B+树记录\n"
        "  qtsvfs gindex <GlobalIndex.data> [--names=tsv]\n"
        "  qtsvfs tree <packageDir> [--depth=N]      用官方 node-index 还原真实文件树\n"
        "  qtsvfs nodes <file.db> [--max=N]          解码 FileNode 与块表\n"
        "  qtsvfs layout <路径> [--package=101] [--depth=N] [--out=tsv] [--no-keyset] [--no-dirs]\n"
        "                                      实时重建名单后打印真实目录树（未定名标 [nameless]）\n"
        "                                      --no-keyset 只用本包节点做哈希闸门（快，但跨包引用定不了名）\n"
        "  qtsvfs export <路径>...|--packages=<root> --out=<目录>\n"
        "                [--nameless] [--no-obsolete] [--limit=N] [--manifest=tsv]\n"
        "                [--threads=N] [--state=tsv] [--dir-prefix=32000] [--normalize] [--no-keyset] [--no-dirs]\n"
        "                                      按现算的真实路径解包落盘（路径先规整，防 ../ 逃逸）\n"
        "                                      多个包共用一棵输出树，同 mount 内同键只解一次\n"
        "                                      多于一个 mount 时每个 mount 各占一个顶层目录\n"
        "                                      --nameless 把未定名落进 [nameless]/前2位/HEX\n"
        "                                      --threads 一个包一个线程并行（0=按 CPU 数），名单在同一线程现算\n"
        "                                      --state 记已完成包，重跑自动续导并吃回旧 manifest\n"
        "                                      --normalize 把载荷改回原厂 SerializedFile（空类型树 -> ett=0），\n"
        "                                      UnityPy 这类通用解析器才读得动对象\n"
        "  qtsvfs alias --export=<导出树> --manifest=<tsv> [--threads=N] [--refs=tsv] [--dry-run]\n"
        "                                      SerializedFile 里 m_StreamData 写的是流节点的规范路径\n"
        "                                      （assets/xx/哈希.resS），名字表多半给的是显示名，\n"
        "                                      这里按路径哈希==节点键对上，补一个规范名硬链接（不占空间）\n"
        "  qtsvfs scan <路径>... [--max=N] [--maxsize=N] [--threads=N]\n"
        "                [--out=候选行tsv] [--missing=盘上无字节tsv] [--buckets] [--markers]\n"
        "                                      实时重建并报覆盖率（全节点/扣无字节/只算活节点三个分母）\n"
        "                                      --buckets 给未定名节点按体内成分分桶\n"
        "                                      --markers 探索模式：统计明文里还有哪些「键: 路径」自声明\n"
        "  qtsvfs keys <路径>... --out=<tsv>       导出全库节点键（哈希、字节、包）给外部脚本\n"
        "  qtsvfs mergenames <tsv>... --out=<名字表>\n"
        "                                      把几份候选行 TSV 并成一张名字表（与实时重建同一套合并规则）\n"
        "  qtsvfs probe <file> [--head=N] [--sample=N] [--block=N]\n"
        "\n"
        "尺寸选项支持 K/M/G 后缀。\n");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const std::vector<std::wstring> wargs = readWideArgs(argc, argv);
    std::vector<std::wstring> toks;  // 去掉程序名，保留宽字符以便还原非 ASCII 路径
    for (std::size_t i = 1; i < wargs.size(); ++i) {
        toks.push_back(wargs[i]);
    }
    if (toks.empty()) {
        usage();
        return 1;
    }
    const std::string sub = wideToUtf8(toks[0]);

    auto utf8 = [](const std::vector<std::wstring>& v) {
        std::vector<std::string> o;
        o.reserve(v.size());
        for (const auto& w : v) {
            o.push_back(wideToUtf8(w));
        }
        return o;
    };
    const std::vector<std::string> rest = utf8(std::vector<std::wstring>(toks.begin() + 1, toks.end()));

    auto isOption = [](const std::string& s) { return s.rfind("--", 0) == 0; };
    // 位置参数：跳过选项，以及（对二级子命令而言）已经被取走的命令名。
    auto positional = [&](std::initializer_list<std::string> used) {
        std::vector<std::filesystem::path> out;
        for (std::size_t i = 0; i < toks.size() - 1; ++i) {
            if (isOption(rest[i])) {
                continue;
            }
            if (std::find(used.begin(), used.end(), rest[i]) != used.end()) {
                continue;
            }
            out.emplace_back(toks[i + 1]);
        }
        return out;
    };

    auto optValue = [&](const std::string& prefix, std::uint64_t& dst, std::uint64_t dflt) {
        dst = dflt;
        for (const auto& s : rest) {
            if (s.rfind(prefix, 0) == 0) {
                const std::size_t eq = s.find('=');
                if (eq == std::string::npos) {
                    return false;
                }
                return parseSize(s.c_str() + eq + 1, dst);
            }
        }
        return true;
    };

    if (sub == "codecs") {
        printCodecs();
        return 0;
    }
    if (sub == "hash") {
        return cmdHash(positional({}));
    }
    if (sub == "kdb") {
        if (rest.empty()) {
            std::fprintf(stderr, "kdb 需要子命令 info|records 与文件\n");
            return 1;
        }
        const std::string mode = rest[0];
        const auto files = positional({mode});
        if (files.empty()) {
            std::fprintf(stderr, "kdb %s 缺少文件参数\n", mode.c_str());
            return 1;
        }
        std::uint64_t max = 20;
        optValue("--max", max, 20);
        const bool hist = std::any_of(rest.begin(), rest.end(),
                                      [](const std::string& s) { return s == "--hist"; });
        if (mode == "info") {
            return cmdKdbInfo(files[0]);
        }
        if (mode == "records") {
            return cmdKdbRecords(files[0], max, hist);
        }
        std::fprintf(stderr, "未知 kdb 子命令: %s\n", mode.c_str());
        return 1;
    }
    if (sub == "tree") {
        const auto files = positional({});
        if (files.empty()) {
            std::fprintf(stderr, "tree 需要包目录参数，如 packages/0\n");
            return 1;
        }
        std::uint64_t depth = 3;
        optValue("--depth", depth, 3);
        std::string outNames;
        for (const auto& t : rest) {
            if (t.rfind("--out", 0) == 0) {
                const std::size_t eq = t.find('=');
                if (eq != std::string::npos) {
                    outNames = t.substr(eq + 1);
                }
            }
        }
        return cmdTree(files[0], depth, outNames);
    }
    if (sub == "extract") {
        const auto files = positional({});
        if (files.size() < 2) {
            std::fprintf(stderr, "extract 需要 <packageDir> <hashHex> [--out=文件]\n");
            return 1;
        }
        std::string outPath, hashHex;
        for (const auto& t : rest) {
            if (t.rfind("--out=", 0) == 0) {
                outPath = t.substr(6);
            } else if (t.size() == 16 && t.find_first_of("0123456789abcdefABCDEF") == 0) {
                hashHex = t;
            }
        }
        if (hashHex.empty()) {
            std::fprintf(stderr, "缺少 16 位十六进制节点哈希\n");
            return 1;
        }
        return cmdExtract(files[0], hashHex, outPath);
    }
    // 位置参数与 --packages/--root 都当根；三种布局（packages、MiniApp_<id>、
    // QtsVFSCache 整根）由 discoverPackages 统一展开。
    auto rootsOf = [&](std::initializer_list<std::string> used) {
        std::vector<std::filesystem::path> roots = positional(used);
        for (const auto& t : rest) {
            if (t.rfind("--packages=", 0) == 0) {
                roots.push_back(utf8ToPath(t.substr(11)));
            } else if (t.rfind("--root=", 0) == 0) {
                roots.push_back(utf8ToPath(t.substr(7)));
            }
        }
        return roots;
    };
    auto prefixesOf = [&]() {
        std::vector<std::string> out;  // 空 = 用 defaultNamePrefixes()
        for (const auto& t : rest) {
            if (t.rfind("--prefix=", 0) != 0) {
                continue;
            }
            const std::string arg = t.substr(9);
            std::size_t pos = 0;
            out.push_back("");  // 不补根
            while (pos < arg.size()) {
                const std::size_t comma = arg.find(',', pos);
                std::string p = arg.substr(pos, comma == std::string::npos ? std::string::npos
                                                                           : comma - pos);
                if (!p.empty()) {
                    if (p.front() == '/') {
                        p.erase(p.begin());
                    }
                    if (p.back() != '/') {
                        p += '/';
                    }
                    out.push_back(std::move(p));
                }
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
        }
        return out;
    };
    const bool noKeyset = std::any_of(rest.begin(), rest.end(),
                                      [](const std::string& t) { return t == "--no-keyset"; });
    // 目录段索引要把整个实例的块解一遍才能建起来（见 Harvest.h 的 DirIndex 注释）。
    // 关掉它快很多，代价是体内自声明的名字有一三成会落到导出根上。
    const bool noDirs = std::any_of(rest.begin(), rest.end(),
                                    [](const std::string& t) { return t == "--no-dirs"; });

    if (sub == "export") {
        const auto roots = rootsOf({});
        std::string outDir, manifest, state, prefix;
        for (const auto& t : rest) {
            if (t.rfind("--out=", 0) == 0) {
                outDir = t.substr(6);
            } else if (t.rfind("--manifest=", 0) == 0) {
                manifest = t.substr(11);
            } else if (t.rfind("--state=", 0) == 0) {
                state = t.substr(8);
            } else if (t.rfind("--dir-prefix=", 0) == 0) {
                prefix = t.substr(13);
            }
        }
        if (roots.empty()) {
            std::fprintf(stderr, "export 需要根目录参数或 --packages=<root>\n");
            return 1;
        }
        if (outDir.empty()) {
            std::fprintf(stderr, "export 需要 --out=<目录>\n");
            return 1;
        }
        std::uint64_t limit = 0;
        optValue("--limit", limit, 0);
        std::uint64_t threads = 0;
        if (!optValue("--threads", threads, 0)) {
            std::fprintf(stderr, "--threads 需要一个数字\n");
            return 1;
        }
        const bool nameless = std::any_of(rest.begin(), rest.end(),
                                          [](const std::string& t) { return t == "--nameless"; });
        const bool skipObsolete = std::any_of(rest.begin(), rest.end(),
                                              [](const std::string& t) { return t == "--no-obsolete"; });
        const bool normalize = std::any_of(rest.begin(), rest.end(),
                                        [](const std::string& t) { return t == "--normalize"; });
        qtsvfs::Library lib = openImport(roots, static_cast<int>(threads), !noKeyset, !noDirs, prefix);
        if (!prefix.empty()) {
            // 只导一批包（皮肤包 32000*、小程序 MiniApp_* 之类），按包目录名或父目录名前缀筛
            lib.pkgs.erase(std::remove_if(lib.pkgs.begin(), lib.pkgs.end(),
                                         [&](const qtsvfs::PackageRef& r) {
                                             const std::string name = r.dir.filename().string();
                                             if (name.rfind(prefix, 0) == 0) return false;
                                             const std::string parent = r.dir.parent_path().filename().string();
                                             return parent.rfind(prefix, 0) != 0;
                                         }),
                          lib.pkgs.end());
        }
        return cmdExport(lib, lib.pkgs, outDir, limit, nameless, skipObsolete, manifest, state,
                         static_cast<int>(threads), normalize, prefixesOf());
    }
    if (sub == "alias") {
        std::string root, manifest, refs;
        for (const auto& t : rest) {
            if (t.rfind("--export=", 0) == 0) {
                root = t.substr(9);
            } else if (t.rfind("--manifest=", 0) == 0) {
                manifest = t.substr(11);
            } else if (t.rfind("--refs=", 0) == 0) {
                refs = t.substr(7);
            }
        }
        if (root.empty() || manifest.empty()) {
            std::fprintf(stderr, "alias 需要 --export=<导出树> 与 --manifest=<tsv>\n");
            return 1;
        }
        const bool dryRun = std::any_of(rest.begin(), rest.end(),
                                         [](const std::string& t) { return t == "--dry-run"; });
        std::uint64_t threads = 0;
        optValue("--threads", threads, 0);
        return cmdAlias(root, manifest, dryRun, static_cast<int>(threads), refs);
    }
    if (sub == "keys") {
        std::string outTsv;
        for (const auto& t : rest) {
            if (t.rfind("--out=", 0) == 0) {
                outTsv = t.substr(6);
            }
        }
        const auto roots = rootsOf({"keys"});
        if (roots.empty() || outTsv.empty()) {
            std::fprintf(stderr, "keys 需要 <根目录> 或 --packages=<root>，并给 --out=<tsv>\n");
            return 1;
        }
        return cmdKeys(qtsvfs::discoverPackages(roots), outTsv);
    }
    if (sub == "layout") {
        const auto roots = rootsOf({});
        if (roots.empty()) {
            std::fprintf(stderr, "layout 需要路径参数，如 packages/101 或 QtsVFSCache 根\n");
            return 1;
        }
        std::string outTsv, packageArg;
        for (const auto& t : rest) {
            if (t.rfind("--out=", 0) == 0) {
                outTsv = t.substr(6);
            } else if (t.rfind("--package=", 0) == 0) {
                packageArg = t.substr(10);
            }
        }
        std::uint64_t depth = 3;
        optValue("--depth", depth, 3);
        std::uint64_t threads = 0;
        optValue("--threads", threads, 0);
        std::filesystem::path prefer;
        for (const auto& r : roots) {
            if (qtsvfs::isPackageDir(r)) {
                prefer = r;  // 传的就是一个包目录时看它自己
                break;
            }
        }
        qtsvfs::Library lib = openImport(roots, static_cast<int>(threads), !noKeyset, !noDirs);
        return cmdLayout(lib, packageArg, prefer, depth, outTsv);
    }
    if (sub == "scan") {
        std::uint64_t max = 0, maxsize = 0;
        if (!optValue("--max", max, 0) || !optValue("--maxsize", maxsize, 0)) {
            std::fprintf(stderr, "选项解析失败\n");
            return 1;
        }
        std::string outTsv, missingTsv;
        for (const auto& t : rest) {
            if (t.rfind("--out=", 0) == 0) {
                outTsv = t.substr(6);
            } else if (t.rfind("--missing=", 0) == 0) {
                missingTsv = t.substr(10);
            }
        }
        const bool buckets = std::any_of(rest.begin(), rest.end(),
                                         [](const std::string& t) { return t == "--buckets"; });
        const bool markers = std::any_of(rest.begin(), rest.end(),
                                         [](const std::string& t) { return t == "--markers"; });
        std::uint64_t threads = 0;
        optValue("--threads", threads, 0);
        const auto roots = rootsOf({});
        if (roots.empty()) {
            std::fprintf(stderr, "scan 需要根目录参数或 --packages=<root>\n");
            return 1;
        }
        qtsvfs::Library lib = openImport(roots, static_cast<int>(threads), !noKeyset, !noDirs);
        if (lib.pkgs.empty()) {
            std::fprintf(stderr, "这些根里没找到包目录（要含与目录同名的 .db 元数据卷）\n");
            return 2;
        }
        ScanRequest req;
        req.maxNodes = max;
        req.maxNodeSize = maxsize;
        req.outTsv = outTsv;
        req.missingTsv = missingTsv;
        req.markers = markers;
        req.buckets = buckets;
        req.prefixes = prefixesOf();
        req.threads = static_cast<int>(threads);
        return cmdScan(lib, req);
    }
    if (sub == "gindex") {
        const auto files = positional({});
        if (files.empty()) {
            std::fprintf(stderr, "gindex 需要 GlobalIndex*.data 文件\n");
            return 1;
        }
        std::string names;
        for (const auto& t : rest) {
            if (t.rfind("--names=", 0) == 0) {
                names = t.substr(8);
            }
        }
        return cmdGindex(files[0], names);
    }
    if (sub == "raw") {
        const auto files = positional({});
        if (files.empty()) {
            std::fprintf(stderr, "raw 需要一个 .db 文件参数\n");
            return 1;
        }
        std::string keyHex, outPath;
        std::uint64_t off = 0, head = 0x100;
        bool hasOff = false;
        for (const auto& t : rest) {
            if (t.rfind("--key=", 0) == 0) {
                keyHex = t.substr(6);
            } else if (t.rfind("--off=", 0) == 0) {
                off = std::strtoull(t.c_str() + 6, nullptr, 0);
                hasOff = true;
            } else if (t.rfind("--out=", 0) == 0) {
                outPath = t.substr(6);
            } else if (t.rfind("--head=", 0) == 0) {
                head = std::strtoull(t.c_str() + 7, nullptr, 0);
            }
        }
        if (keyHex.empty() && !hasOff) {
            std::fprintf(stderr, "raw 需要 --key=HEX16 或 --off=偏移\n");
            return 1;
        }
        return cmdRaw(files[0], keyHex, off, hasOff, outPath, head);
    }
    if (sub == "nodes") {
        const auto files = positional({});
        if (files.empty()) {
            std::fprintf(stderr, "nodes 需要一个 .db 文件参数\n");
            return 1;
        }
        std::uint64_t max = 20;
        optValue("--max", max, 20);
        return cmdNodes(files[0], max, std::any_of(rest.begin(), rest.end(),
                                                    [](const std::string& t) { return t == "--fields"; }));
    }
    if (sub == "mergenames") {
        std::string outTsv;
        for (const auto& t : rest) {
            if (t.rfind("--out=", 0) == 0) {
                outTsv = t.substr(6);
            }
        }
        const auto files = positional({});
        if (files.empty() || outTsv.empty()) {
            std::fprintf(stderr, "用法: qtsvfs mergenames <harvest.tsv>... --out=<名字表>\n");
            return 1;
        }
        return cmdMergeNames(files, outTsv);
    }
    if (sub == "probe") {
        std::uint64_t head = 0x100, sample = 16ull * 1024 * 1024, block = 64ull * 1024;
        if (!optValue("--head", head, 0x100) || !optValue("--sample", sample, 16ull * 1024 * 1024) ||
            !optValue("--block", block, 64ull * 1024)) {
            std::fprintf(stderr, "选项解析失败\n");
            return 1;
        }
        const auto files = positional({});
        if (files.empty()) {
            std::fprintf(stderr, "probe 需要一个文件参数\n");
            return 1;
        }
        return cmdProbe(files[0], head, sample, block);
    }
    usage();
    return 1;
}
