#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "qtsvfs/codec/Codec.h"
#include "qtsvfs/Catalog.h"
#include "qtsvfs/Export.h"
#include "qtsvfs/Names.h"
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
                                .substr(0, 8)
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

int cmdNodes(const std::filesystem::path& p, std::uint64_t limit) {
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
                static_cast<unsigned long long>(other), err.empty() ? "" : ("  遍历警告: " + err));
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

// 批量解块并在明文里找自声明标记：标记串只有 hash(形式)==本节点哈希 才算「自声明」，
// 命中包内其他节点算「引用」，其余算未验证（可能是包内相对路径，缺挂载前缀）。
struct ScanStats {
    std::uint64_t nodes = 0, decoded = 0, failed = 0, skipped = 0;
    std::uint64_t withMarker = 0, candidates = 0, selfHit = 0, refHit = 0, loHit = 0, unverified = 0;
    std::uint64_t sizeMismatch = 0;
    std::uint64_t ratio[5] = {0, 0, 0, 0, 0};  // <0.5 0.5-0.7 0.7-0.85 0.85-0.95 >0.95
};

double printableRatio(const std::vector<std::uint8_t>& b) {
    if (b.empty()) {
        return 0.0;
    }
    const std::size_t n = std::min<std::size_t>(b.size(), 64 * 1024);
    std::size_t ok = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = b[i];
        if ((c >= 0x20 && c <= 0x7e) || c == '\t' || c == '\n' || c == '\r') ok++;
    }
    return static_cast<double>(ok) / static_cast<double>(n);
}

// 通用标记键：2..24 个「词字符」+ 词干（如 Path）+ : 或 = ，用于统计有哪些自声明格式。
bool markerKeyAt(const std::string& t, std::size_t pos, std::size_t& keyStart, std::size_t& keyEnd) {
    std::size_t i = 0;
    while (i < pos && i < 24) {
        const char c = t[pos - i - 1];
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '_' || c == '-';
        if (!keep) break;
        ++i;
    }
    if (i < 1) {
        return false;
    }
    keyStart = pos - i;
    keyEnd = pos;
    return true;
}

// 裸路径候选：资源/脚本内容里直接写出的路径串（不依赖前面的键名），
// 用 GlobalIndex 命中率判定它是不是真的 VFS 路径（u32 命中噪声底约 0.037%）。
struct BareStats {
    std::uint64_t cand = 0, hitLo = 0, hitHi = 0, selfHit = 0, refHit = 0;
    std::unordered_map<std::string, std::uint64_t> formHit;
    std::unordered_map<std::string, std::uint64_t> segFreq;
    std::unordered_map<std::string, std::uint64_t> segHit;
    std::unordered_map<std::uint16_t, std::uint64_t> hitPkg;
    std::vector<std::string> examples;
    // 已定名：路径哈希命中包内某个节点（含自身），可直接当真实文件名用
    std::unordered_map<std::uint64_t, std::string> named;
    // catalog 定名：同一条记录里「资源真名 + 声明路径」成对出现，路径过了哈希闸门后，
    // 把真名挂到那个节点上。比 named 更接近用户要的「不是哈希树」。
    std::unordered_map<std::uint64_t, std::string> namedReal;
    // 节点体内只有一个资源名字段的单资源节点：名字来自「它自己就是那个资源的数据」
    std::unordered_map<std::uint64_t, std::string> selfNames;
    std::uint64_t catalogEntries = 0;
    std::uint64_t namedSelf = 0;
    // 候选按「末段扩展名 / 无扩展」分桶，用来看还有哪些文件类型、为什么没命中
    std::unordered_map<std::string, std::array<std::uint64_t, 2>> extStats;  // [候选, 命中]
    std::uint64_t noExtCand = 0, noExtHit = 0;
};

bool hasExtension(const std::string& t) {
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

std::string firstSegment(const std::string& t) {
    std::string s = t;
    if (!s.empty() && s[0] == '/') {
        s.erase(s.begin());
    }
    const std::size_t slash = s.find('/');
    return slash == std::string::npos ? s : s.substr(0, slash);
}

// 路径规范化形：哈希闸门一律用小写（官方 210 条白名单用 hash64(小写路径) 校验 210/210
// 命中，保留大小写只有 2 条），所以形态维度只剩「要不要折叠 //」与「挂哪个根」。
// 根前缀来自实测：容器明文里的 .bytes/.txt/.mp4/.png 都是相对挂载点写的，
// 补上 /RawAssets/Domestic 后 GlobalIndex 命中率从 0 跳到 200/201、166/182、47/64。
std::vector<std::pair<std::string, std::string>> buildForms(const std::string& tok,
                                                            const std::vector<std::string>& prefixes) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    };
    auto collapse = [](std::string s) {
        std::string o;
        o.reserve(s.size());
        for (std::size_t k = 0; k < s.size(); ++k) {
            if (s[k] == '/' && k + 1 < s.size() && s[k + 1] == '/') {
                continue;
            }
            o += s[k];
        }
        return o;
    };
    std::vector<std::pair<std::string, std::string>> out;
    for (int v = 0; v < 2; ++v) {
        std::string base = v ? collapse(tok) : tok;
        base = lower(base);
        while (!base.empty() && base[0] == '/') {
            base.erase(base.begin());
        }
        for (const auto& p : prefixes) {
            std::string name = p.empty() ? "裸路径" : p;
            if (v) {
                name += "+折叠";
            }
            std::string full = "/" + p + base;
            // 已经规范过的串，"原样"和"折叠"会算出同一个值，重复计数会虚高命中率
            bool dup = false;
            for (const auto& e : out) {
                if (e.second == full) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            out.emplace_back(std::move(name), std::move(full));
        }
    }
    return out;
}

// 从明文里取路径候选：除了 ASCII 串，还要取 UTF-16LE（Windows/Wwise/Unity 序列化里
// 路径常以两字节字符存，中间夹 0x00，ASCII 扫描整段都会漏掉）。
std::vector<std::string> grabPathTokens(const std::string& text) {
    static const auto ok = [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
               c == '_' || c == '-' || c == '.' || c == '/' || c == '+';
    };
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!ok(text[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && ok(text[j]) && j - i < 260) {
            ++j;
        }
        out.push_back(text.substr(i, j - i));
        i = j;
    }
    // UTF-16LE：可打印字节后紧跟 0x00
    i = 0;
    while (i + 1 < text.size()) {
        if (!ok(text[i]) || text[i + 1] != '\0') {
            ++i;
            continue;
        }
        std::string tok;
        std::size_t k = i;
        while (k + 1 < text.size() && ok(text[k]) && text[k + 1] == '\0' && tok.size() < 260) {
            tok += text[k];
            k += 2;
        }
        i = k > i ? k : i + 1;
        if (tok.size() >= 2 && (tok.find('/') != std::string::npos)) {
            out.push_back(std::move(tok));
        }
    }
    return out;
}

void scanBarePaths(const std::string& text, const qtsvfs::FileNode* node, BareStats& bs,
                   const qtsvfs::GlobalIndex* gi,
                   const std::unordered_map<std::uint64_t, qtsvfs::FileNode>& nodes,
                   std::ostream* os, bool recordAll,
                   const std::vector<std::string>& prefixes,
                   const std::unordered_set<std::uint64_t>* keyset) {
    for (const std::string& tok : grabPathTokens(text)) {
        if (tok.size() < 8 || tok.find('/') == std::string::npos) {
            continue;
        }
        const bool ext = hasExtension(tok);
        std::string bucket;
        if (ext) {
            bucket = tok.substr(tok.rfind('.') + 1);
            if (bucket.size() > 8) {
                continue;
            }
        } else {
            bucket = "(无扩展)";
            ++bs.noExtCand;
        }
        ++bs.cand;
        bs.extStats[bucket][0]++;
        const std::string seg = firstSegment(tok);
        bs.segFreq[seg]++;
        bool anyHit = false;
        for (const auto& [name, cand] : buildForms(tok, prefixes)) {
            const std::uint64_t h = qtsvfs::calcHashCode64(cand);
            bool hit = false;
            if (gi) {
                const std::uint16_t pid =
                    gi->findPackage(static_cast<std::uint32_t>(h & 0xFFFFFFFFu));
                if (pid != 0xFFFF) {
                    ++bs.hitLo;
                    bs.hitPkg[pid]++;
                }
                if (gi->findPackage(static_cast<std::uint32_t>(h >> 32)) != 0xFFFF) {
                    ++bs.hitHi;
                }
            }
            const bool self = h == node->hash;
            const bool ref = !self && (nodes.count(h) != 0 || (keyset && keyset->count(h) != 0));
            if (self) {
                hit = true;
                ++bs.selfHit;
            }
            if (ref) {
                hit = true;
                ++bs.refHit;
            }
            if (hit) {
                anyHit = true;
                bs.formHit[name]++;
                bs.segHit[seg]++;
                bs.extStats[bucket][1]++;
                if (!ext) {
                    ++bs.noExtHit;
                }
                bs.named.try_emplace(h, cand);  // 只有整 64 位节点键相等才入库，噪声可忽略
                if (bs.examples.size() < 40) {
                    bs.examples.push_back(name + " | " + cand.substr(0, 110));
                }
            }
        }
        if (os && (anyHit || recordAll)) {
            *os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << node->hash
                << (anyHit ? "\tbare\t" : "\tcand\t") << seg << '\t' << tok << '\n'
                << std::dec;
        }
    }
}

// catalog 记录：名字来自同一条记录，节点键来自声明路径的哈希闸门，两者都成立才记。
void scanCatalog(const std::string& text, const qtsvfs::FileNode* node, BareStats& bs,
                 const std::unordered_map<std::uint64_t, qtsvfs::FileNode>& nodes, std::ostream* os,
                 const std::unordered_set<std::uint64_t>* keyset) {
    const auto entries = qtsvfs::parseCatalog(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size());
    bs.catalogEntries += entries.size();
    for (const auto& e : entries) {
        std::string lower = e.path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        while (!lower.empty() && lower[0] == '/') {
            lower.erase(lower.begin());
        }
        const std::string cand = "/" + lower;
        const std::uint64_t h = qtsvfs::calcHashCode64(cand);
        if (h != node->hash && nodes.count(h) == 0 && !(keyset && keyset->count(h))) {
            continue;  // 声明的路径在这份数据里没有对应节点，名字也就无处可挂
        }
        std::string real = e.name;
        for (char& c : real) {
            if (c == '/' || c == '\\') {
                c = '_';
            }
        }
        real += "." + e.ext;
        const bool fresh = bs.namedReal.try_emplace(h, real).second;
        if (os && fresh) {
            *os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h << "\treal\tcatalog\t"
                << real << '\n'
                << std::dec;
        }
    }
}

// 脚本节点的自声明模块名：JS 运行时在文件头写 `InGamePath: JS/…/x.mjs`。
// 这条串以前被判过「不是名字来源」，理由是它哈希不到任何节点键 —— 那是把它当成
// 「指向别人的路径」才有的要求。它其实是「我这个文件叫什么」，配对依据是包含关系，
// 不需要哈希闸门；只有在整块明文里只出现 1 个不同取值时才采纳，多了就是引用列表。
void scanInGamePath(const std::string& text, const qtsvfs::FileNode* node, BareStats& bs,
                    std::ostream* os) {
    static const std::string key = "InGamePath:";
    std::string val;
    std::size_t distinct = 0;
    for (std::size_t p = text.find(key); p != std::string::npos; p = text.find(key, p + key.size())) {
        std::size_t s = p + key.size();
        while (s < text.size() && text[s] == ' ') {
            ++s;
        }
        std::size_t e = s;
        static const auto okc = [](char c) {
            const unsigned char u = static_cast<unsigned char>(c);
            return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
                   c == '_' || c == '-' || c == '.' || c == '/' || c == '+';
        };
        while (e < text.size() && okc(text[e]) && e - s < 240) {
            ++e;
        }
        std::string v = text.substr(s, e - s);
        if (v.size() < 4) {
            continue;
        }
        if (val.empty()) {
            val = v;
            ++distinct;
        } else if (val != v) {
            ++distinct;
            break;
        }
    }
    if (distinct != 1) {
        return;
    }
    // 声明形如 `InGamePath: JS//GameScripts/…`，双斜杠是运行时拼出来的，折叠掉。
    std::string name;
    for (std::size_t i = 0; i < val.size(); ++i) {
        if (val[i] == '/' && i + 1 < val.size() && val[i + 1] == '/') {
            continue;
        }
        name += val[i];
    }
    if (name.empty() || name[0] != '/') {
        name = "/" + name;
    }
    if (bs.selfNames.try_emplace(node->hash, name).second && os) {
        *os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << node->hash
            << "\tobject\tingamepath\t" << name << '\n'
            << std::dec;
    }
}

int cmdScan(const std::filesystem::path& pkgDir, std::uint64_t maxNodes, std::uint64_t maxNodeSize,
            const std::string& outTsv, bool append, bool showAll, bool trace, bool bare,
            const qtsvfs::GlobalIndex* gi, const std::vector<std::string>& prefixes,
            const std::unordered_set<std::uint64_t>* keyset, std::ostream* missing,
            const qtsvfs::NameTable* known) {
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
    std::vector<const qtsvfs::FileNode*> list;
    list.reserve(pkg.nodes().size());
    for (const auto& [h, n] : pkg.nodes()) {
        (void)h;
        list.push_back(&n);
    }
    std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
        if (a->size != b->size) return a->size < b->size;
        return a->hash < b->hash;
    });

    std::unordered_map<std::string, std::uint64_t> keyFreq;
    std::unordered_map<std::string, std::string> keyExample;
    std::unordered_map<std::string, std::uint64_t> failReason;
    std::unordered_map<std::string, std::uint64_t> unnamedKind;  // 有字节却没定名的原因分桶
    std::vector<std::array<std::uint64_t, 5>> badSizes;  // hash, 声明, 实得, method, 块数
    BareStats bs;
    ScanStats st;
    std::ofstream os;
    if (!outTsv.empty()) {
        os.open(outTsv, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    }

    // 手写 token 扫描：regex 的嵌套量词在长文本上会回溯爆栈。
    auto isPathChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.' || c == '/' || c == '+';
    };

    for (std::size_t idx = 0; idx < list.size() && st.nodes < maxNodes; ++idx) {
        const qtsvfs::FileNode* node = list[idx];
        ++st.nodes;
        if (node->size > maxNodeSize) {
            ++st.skipped;
            continue;
        }
        std::vector<std::uint8_t> blob;
        std::string de;
        if (trace) {
            std::fprintf(stderr, "trace %016llX size=%llu blocks=%u method=%u\n",
                         static_cast<unsigned long long>(node->hash),
                         static_cast<unsigned long long>(node->size), node->blockCount,
                         node->blocks.empty() ? 0u
                             : static_cast<unsigned>(node->blocks[0].packed & 0xFFu));
        }
        try {
            if (!pkg.readBlob(*node, blob, de)) {
                ++st.failed;
                failReason[de.substr(0, 72)]++;
                // 「没有数据卷含该节点的块」= 这份按需下载缓存里根本没它的字节，
                // 列出来才能把「定名率」的分母说清楚，也才查得出是不是索引漏了。
                if (missing && de.find("没有数据卷") != std::string::npos) {
                    *missing << std::hex << std::uppercase << std::setw(16) << std::setfill('0')
                             << node->hash << '\t' << std::dec << node->size << '\t'
                             << pkgDir.filename().string() << '\n';
                }
                continue;
            }
        } catch (const std::exception& e) {
            ++st.failed;
            failReason[std::string("exception: ") + e.what()]++;
            continue;
        }
        ++st.decoded;
        if (blob.size() != node->size) {
            ++st.sizeMismatch;
            if (badSizes.size() < 6) {
                badSizes.push_back({node->hash, node->size, blob.size(),
                                    node->blocks.empty() ? 0u
                                                          : static_cast<unsigned>(
                                                                node->blocks[0].packed & 0xFFu),
                                    node->blockCount});
            }
        }
        const double r = printableRatio(blob);
        st.ratio[r < 0.5 ? 0 : r < 0.7 ? 1 : r < 0.85 ? 2 : r < 0.95 ? 3 : 4]++;
        // 标记可能藏在二进制包装的明文段里，因此不按可打印率设门槛。
        const std::string text(reinterpret_cast<const char*>(blob.data()), blob.size());
        const std::size_t scanLimit = std::min<std::size_t>(text.size(), 4u << 20);
        bool nodeHasMarker = false;
        std::unordered_set<std::string> seen;
        // 通用发现模式：任意 「键: 路径」/「键=路径」 都算候选，用来看还有哪些自声明格式。
        for (std::size_t sep = text.find(':', 0); sep != std::string::npos && sep < scanLimit;
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
            nodeHasMarker = true;
            if (!seen.insert(key + "\x01" + tok).second) {
                continue;
            }
            ++st.candidates;
            keyFreq[key]++;
            keyExample.try_emplace(key, tok);
            int hit = 0;  // 1=本节点自声明 2=指向包内其他节点 3=低32位命中(全局索引 fileHash)
            for (int form = 0; form < 2; ++form) {
                const std::string base = form ? "/" + tok : tok;
                for (int lower = 0; lower < 2; ++lower) {
                    std::string sl = base;
                    if (lower) {
                        std::transform(sl.begin(), sl.end(), sl.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                    }
                    const std::uint64_t h = qtsvfs::calcHashCode64(sl);
                    if (h == node->hash) {
                        hit = 1;
                    } else if (hit != 1 && pkg.nodes().count(h)) {
                        hit = 2;
                    } else if (hit == 0 && (h & 0xFFFFFFFFu) == (node->hash & 0xFFFFFFFFu)) {
                        hit = 3;
                    }
                }
            }
            if (hit == 1) ++st.selfHit;
            else if (hit == 2) ++st.refHit;
            else if (hit == 3) ++st.loHit;
            else ++st.unverified;
            if (os && (hit || showAll)) {
                os << std::hex << std::uppercase << std::setw(16) << std::setfill('0')
                   << node->hash << (hit == 1 ? "\tself\t" : hit == 2   ? "\tref\t"
                                              : hit == 3 ? "\tlo32\t"   : "\t-\t")
                   << key << '\t' << tok << '\n' << std::dec;
            }
        }
        if (nodeHasMarker) {
            ++st.withMarker;
        }
        if (bare) {
            scanBarePaths(text, node, bs, gi, pkg.nodes(), os ? &os : nullptr, showAll, prefixes,
                          keyset);
            scanCatalog(text, node, bs, pkg.nodes(), os ? &os : nullptr, keyset);
            // 顺序即优先级：脚本节点先看运行时自声明的模块路径，再看序列化资源名。
            scanInGamePath(text, node, bs, os ? &os : nullptr);
            qtsvfs::SelfName sn;
            if (qtsvfs::parseSelfName(blob.data(), blob.size(), sn) &&
                bs.selfNames
                    .try_emplace(node->hash,
                                 "/" + (sn.dir.empty() ? "" : sn.dir + "/") + sn.name)
                    .second &&
                os) {
                os << std::hex << std::uppercase << std::setw(16) << std::setfill('0')
                   << node->hash << (sn.confident ? "\tobject\tselfname\t" : "\tobject\tobjname\t")
                   << "/" << (sn.dir.empty() ? "" : sn.dir + "/") << sn.name << '\n'
                   << std::dec;
            }
        }
        if (known && !known->count(node->hash)) {
            const auto cs = qtsvfs::measureCatalog(blob.data(), blob.size());
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
            ++unnamedKind[kind];
        }
        if (st.nodes % 1000 == 0) {
            std::printf("  … 已扫 %llu 节点，含标记节点 %llu，自声明 %llu，引用 %llu\n",
                        static_cast<unsigned long long>(st.nodes),
                        static_cast<unsigned long long>(st.withMarker),
                        static_cast<unsigned long long>(st.selfHit),
                        static_cast<unsigned long long>(st.refHit));
            std::fflush(stdout);
        }
    }
    std::printf("包 %s: 节点扫描=%llu 解压失败=%llu 超限跳过=%llu 含标记节点=%llu 标记串=%llu\n",
                pkgDir.filename().string().c_str(), static_cast<unsigned long long>(st.nodes),
                static_cast<unsigned long long>(st.failed),
                static_cast<unsigned long long>(st.skipped),
                static_cast<unsigned long long>(st.withMarker),
                static_cast<unsigned long long>(st.candidates));
    std::printf("  哈希自校验: 本节点自声明=%llu 指向包内其他节点=%llu 低32位命中=%llu 无匹配=%llu\n",
                static_cast<unsigned long long>(st.selfHit),
                static_cast<unsigned long long>(st.refHit),
                static_cast<unsigned long long>(st.loHit),
                static_cast<unsigned long long>(st.unverified));
    std::printf("  可打印率分布 <0.5/0.5-0.7/0.7-0.85/0.85-0.95/>0.95 = %llu/%llu/%llu/%llu/%llu，"
                "解出长度与声明不符=%llu\n",
                static_cast<unsigned long long>(st.ratio[0]),
                static_cast<unsigned long long>(st.ratio[1]),
                static_cast<unsigned long long>(st.ratio[2]),
                static_cast<unsigned long long>(st.ratio[3]),
                static_cast<unsigned long long>(st.ratio[4]),
                static_cast<unsigned long long>(st.sizeMismatch));
    for (const auto& b : badSizes) {
        std::printf("    尺寸不符 %016llX 声明=%llu 实得=%llu method=%llu 块数=%llu\n",
                    static_cast<unsigned long long>(b[0]),
                    static_cast<unsigned long long>(b[1]), static_cast<unsigned long long>(b[2]),
                    static_cast<unsigned long long>(b[3]),
                    static_cast<unsigned long long>(b[4]));
    }
    std::vector<std::pair<std::string, std::uint64_t>> fails(failReason.begin(), failReason.end());
    std::sort(fails.begin(), fails.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    for (std::size_t i = 0; i < fails.size() && i < 6; ++i) {
        std::printf("    失败 x%llu: %s\n", static_cast<unsigned long long>(fails[i].second),
                    fails[i].first.c_str());
    }
    std::vector<std::pair<std::string, std::uint64_t>> keys(keyFreq.begin(), keyFreq.end());
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("  声明键 Top15（次数 | 例子）:\n");
    for (std::size_t i = 0; i < keys.size() && i < 15; ++i) {
        std::printf("    %-22s %-7llu %s\n", keys[i].first.c_str(),
                    static_cast<unsigned long long>(keys[i].second),
                    keyExample.count(keys[i].first) ? keyExample[keys[i].first].substr(0, 72).c_str()
                                                    : "");
    }
    if (bare) {
        std::printf("  哈希可定名节点=%llu（本节点 %llu / 包内他节点 %llu）\n",
                    static_cast<unsigned long long>(bs.named.size()),
                    static_cast<unsigned long long>(bs.selfHit),
                    static_cast<unsigned long long>(bs.refHit));
        std::printf("  catalog 记录=%llu  其中真名可挂到节点键=%llu  体内单资源名节点=%llu\n",
                    static_cast<unsigned long long>(bs.catalogEntries),
                    static_cast<unsigned long long>(bs.namedReal.size()),
                    static_cast<unsigned long long>(bs.selfNames.size()));
        if (os) {
            for (const auto& [h, p] : bs.named) {
                os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h
                   << "\tnamed\t\t" << p << '\n'
                   << std::dec;
            }
        }
        const double noise = gi ? (static_cast<double>(bs.cand) * 7.0 *
                                   static_cast<double>(gi->fileCount()) / 4294967296.0)
                                : 0.0;
        std::printf("  裸路径候选=%llu  GlobalIndex低32命中=%llu  高32命中=%llu  本节点=%llu  包内他节点=%llu"
                    "（随机噪声量级约 %.1f）\n",
                    static_cast<unsigned long long>(bs.cand),
                    static_cast<unsigned long long>(bs.hitLo),
                    static_cast<unsigned long long>(bs.hitHi),
                    static_cast<unsigned long long>(bs.selfHit),
                    static_cast<unsigned long long>(bs.refHit), noise);
        std::vector<std::pair<std::uint16_t, std::uint64_t>> pkgs(bs.hitPkg.begin(), bs.hitPkg.end());
        std::sort(pkgs.begin(), pkgs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("  GlobalIndex 命中落在哪些全局包号 Top6:");
        for (std::size_t i = 0; i < pkgs.size() && i < 6; ++i) {
            std::printf(" %u×%llu", pkgs[i].first, static_cast<unsigned long long>(pkgs[i].second));
        }
        std::printf("\n");
        std::vector<std::pair<std::string, std::uint64_t>> forms(bs.formHit.begin(), bs.formHit.end());
        std::sort(forms.begin(), forms.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& f : forms) {
            std::printf("    形式 %-12s %llu\n", f.first.c_str(),
                        static_cast<unsigned long long>(f.second));
        }
        std::vector<std::pair<std::string, std::array<std::uint64_t, 2>>> exts(bs.extStats.begin(),
                                                                              bs.extStats.end());
        std::sort(exts.begin(), exts.end(), [](const auto& a, const auto& b) {
            if (a.second[0] != b.second[0]) return a.second[0] > b.second[0];
            return a.first < b.first;
        });
        std::printf("  候选按扩展名（候选 | 哈希命中）Top14:\n");
        for (std::size_t i = 0; i < exts.size() && i < 14; ++i) {
            std::printf("    .%-12s %-9llu %llu\n", exts[i].first.c_str(),
                        static_cast<unsigned long long>(exts[i].second[0]),
                        static_cast<unsigned long long>(exts[i].second[1]));
        }
        std::printf("  无扩展候选=%llu 命中=%llu\n",
                    static_cast<unsigned long long>(bs.noExtCand),
                    static_cast<unsigned long long>(bs.noExtHit));
        std::vector<std::pair<std::string, std::uint64_t>> segs(bs.segFreq.begin(), bs.segFreq.end());
        std::sort(segs.begin(), segs.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        std::printf("  首段 Top12（候选数 | 命中数）:\n");
        for (std::size_t i = 0; i < segs.size() && i < 12; ++i) {
            std::printf("    %-28s %-8llu %llu\n", segs[i].first.substr(0, 28).c_str(),
                        static_cast<unsigned long long>(segs[i].second),
                        static_cast<unsigned long long>(bs.segHit.count(segs[i].first)
                                                            ? bs.segHit[segs[i].first]
                                                            : 0));
        }
        for (std::size_t i = 0; i < bs.examples.size() && i < 12; ++i) {
            std::printf("    例: %s\n", bs.examples[i].c_str());
        }
    }
    if (os) {
        std::printf("  → 命中清单: %s\n", outTsv.c_str());
    }
    if (known && !unnamedKind.empty()) {
        std::vector<std::pair<std::string, std::uint64_t>> kinds(unnamedKind.begin(),
                                                                 unnamedKind.end());
        std::sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
        std::uint64_t sum = 0;
        for (const auto& [k, v] : kinds) {
            sum += v;
        }
        std::printf("  有字节但未定名 %llu 个，按原因分桶:\n", static_cast<unsigned long long>(sum));
        for (std::size_t i = 0; i < kinds.size() && i < 12; ++i) {
            std::printf("    %-40s %llu\n", kinds[i].first.c_str(),
                        static_cast<unsigned long long>(kinds[i].second));
        }
    }
    return 0;
}

int cmdLayout(const std::filesystem::path& pkgDir, const std::string& namesFile,
              std::uint64_t depthLimit, const std::string& outTsv) {
    qtsvfs::NameTable table;
    qtsvfs::SourceTable sources;
    std::string err;
    if (!qtsvfs::loadNameTable(utf8ToPath(namesFile), table, err, &sources)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    qtsvfs::makeUnique(table);
    qtsvfs::Package pkg;
    if (!pkg.open(pkgDir, err) || !pkg.loadNodes(err)) {
        std::fprintf(stderr, "打开包或读 FileNode 失败: %s\n", err.c_str());
        return 2;
    }
    std::vector<const qtsvfs::FileNode*> nodes;
    nodes.reserve(pkg.nodes().size());
    for (const auto& [h, n] : pkg.nodes()) {
        (void)h;
        nodes.push_back(&n);
    }
    qtsvfs::TreeNode root;
    qtsvfs::TreeStats st;
    qtsvfs::buildTree(nodes, table, root, st, &sources);
    qtsvfs::sortTree(root);
    std::printf("包 %s：节点 %zu，真名 %llu，未定名 %llu，未压总字节 %llu（名字表 %zu 条）\n",
                pkgDir.filename().string().c_str(), nodes.size(),
                static_cast<unsigned long long>(st.named),
                static_cast<unsigned long long>(st.nameless),
                static_cast<unsigned long long>(st.bytes), table.size());
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
               << (r.named ? "name" : "hash") << '\n';
        }
        std::printf("  → 文件树清单 %zu 行: %s\n", rows.size(), outTsv.c_str());
    }
    return 0;
}

int cmdExport(const std::filesystem::path& pkgDir, const std::string& namesFile,
              const std::string& outDir, std::uint64_t limit, bool nameless) {
    qtsvfs::NameTable table;
    std::string err;
    if (!qtsvfs::loadNameTable(utf8ToPath(namesFile), table, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    qtsvfs::makeUnique(table);
    qtsvfs::Package pkg;
    if (!pkg.open(pkgDir, err) || !pkg.loadNodes(err)) {
        std::fprintf(stderr, "打开包或读 FileNode 失败: %s\n", err.c_str());
        return 2;
    }
    std::printf("包 %s：%zu 节点，名字表 %zu 条，导出到 %s\n", pkgDir.filename().string().c_str(),
                pkg.nodes().size(), table.size(), outDir.c_str());
    const qtsvfs::ExportResult r =
        qtsvfs::exportPackage(pkg, table, utf8ToPath(outDir), limit, nullptr,
                              [](const qtsvfs::ExportResult& cur) {
                                  if (cur.files % 500 == 0) {
                                      std::printf("  … 已导出 %llu 个 (%llu MB)\n",
                                                  static_cast<unsigned long long>(cur.files),
                                                  static_cast<unsigned long long>(cur.bytes >> 20));
                                      std::fflush(stdout);
                                  }
                              },
            nameless);
    std::printf("  → 导出 %llu 个文件共 %llu 字节，失败 %llu，改名 %llu，无名跳过 %llu%s\n",
                static_cast<unsigned long long>(r.files),
                static_cast<unsigned long long>(r.bytes),
                static_cast<unsigned long long>(r.failed),
                static_cast<unsigned long long>(r.renamed),
                static_cast<unsigned long long>(r.skipped),
                r.cancelled ? "  [已取消]" : "");
    return r.error.empty() ? 0 : 3;
}


bool loadKeyset(const std::string& file, std::unordered_set<std::uint64_t>& out) {
    std::ifstream in(file);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 16) {
            continue;
        }
        out.insert(std::strtoull(line.substr(0, 16).c_str(), nullptr, 16));
    }
    return !out.empty();
}

// 全库节点键清单：名字跨包引用时，同包节点表不够用（实测 .bytes/.txt 的宿主节点在别的包），
// 所以先把所有元数据卷的 FileNode 键导成一张集合表，供 scan 当闸门。
int cmdKeys(const std::vector<std::filesystem::path>& dirs, const std::string& outTsv) {
    std::ofstream os(outTsv, std::ios::binary | std::ios::trunc);
    if (!os) {
        std::fprintf(stderr, "写不出 %s\n", outTsv.c_str());
        return 2;
    }
    std::uint64_t total = 0, bad = 0;
    for (const auto& dir : dirs) {
        qtsvfs::Package pkg;
        std::string err;
        if (!pkg.open(dir, err) || !pkg.loadNodes(err)) {
            ++bad;
            continue;
        }
        for (const auto& [h, n] : pkg.nodes()) {
            os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h << '\t'
               << std::dec << n.size << '\t' << dir.filename().string() << '\n';
        }
        total += pkg.nodes().size();
    }
    std::printf("导出 %llu 个节点键（%zu 个目录，跳过 %llu）→ %s\n",
                static_cast<unsigned long long>(total), dirs.size(),
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

// 把多份 scan 产出合成一张名字表，同一节点键按来源优先级只留一条：
//   0 real   —— catalog 记录里的真名（路径过了哈希闸门，名字来自同一条记录）
//   1 object —— 节点体内自声明（ingamepath/selfname/objname），配对依据是包含关系
//   2 named  —— 裸路径哈希定名，只有哈希路径可看
// object 缺目录段而同一键又有 named 时，目录用 named 的（哈希验证过的挂载位置）、
// 文件段用 object 的（资源自己的名字）。最后统一做同名消歧。
int cmdMergeNames(const std::vector<std::filesystem::path>& ins, const std::string& outTsv) {
    struct Best {
        int rank = 9;
        std::string tag;
        std::string sub;
        std::string path;
    };
    std::unordered_map<std::uint64_t, Best> map;
    std::unordered_map<std::uint64_t, std::string> dirOf;
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
            std::string tag, sub, path;
            std::size_t t = 17;
            for (int col = 0; col < 3; ++col) {
                const std::size_t nx = col == 2 ? std::string::npos : line.find('\t', t);
                const std::string field =
                    nx == std::string::npos ? line.substr(t) : line.substr(t, nx - t);
                if (col == 0) {
                    tag = field;
                } else if (col == 1) {
                    sub = field;
                } else {
                    path = field;
                }
                if (nx == std::string::npos) {
                    break;
                }
                t = nx + 1;
            }
            if (path.empty()) {
                continue;
            }
            if (tag != "real" && tag != "object" && tag != "named") {
                continue;
            }
            const int rank = tag == "real" ? 0 : tag == "object" ? 1 : 2;
            const std::uint64_t h = std::strtoull(line.substr(0, 16).c_str(), nullptr, 16);
            if (h == 0) {
                continue;
            }
            if (tag == "named") {
                const std::size_t slash = path.rfind('/');
                if (slash != std::string::npos) {
                    dirOf.try_emplace(h, path.substr(0, slash));
                }
            }
            auto it = map.find(h);
            if (it == map.end() || rank < it->second.rank) {
                map[h] = Best{rank, tag, sub, path};
                bySource[tag + "/" + sub]++;
            } else {
                bySource["被更高优先级覆盖"]++;
            }
        }
    }

    for (auto& [h, b] : map) {
        if (b.rank == 2) {
            continue;  // named 本身就是完整路径，不需要借目录
        }
        const std::size_t slash = b.path.rfind('/');
        if (slash != std::string::npos && slash != 0) {
            continue;  // 这条名字已经带了目录段
        }
        const auto d = dirOf.find(h);
        if (d != dirOf.end()) {
            b.path = d->second + b.path;
        }
    }

    qtsvfs::NameTable table;
    qtsvfs::SourceTable sources;
    for (const auto& [h, b] : map) {
        table.emplace(h, b.path);
        sources.emplace(h, b.sub.empty() ? b.tag : b.tag + ":" + b.sub);
    }
    qtsvfs::makeUnique(table);
    std::ofstream os(utf8ToPath(outTsv));
    if (!os) {
        std::fprintf(stderr, "写不出 %s\n", outTsv.c_str());
        return 3;
    }
    for (const auto& [h, p] : table) {
        const auto s = sources.find(h);
        const std::string tag = s == sources.end() ? "named" : s->second.substr(0, s->second.find(':'));
        const std::string sub =
            s == sources.end() || s->second.find(':') == std::string::npos
                ? "" : s->second.substr(s->second.find(':') + 1);
        os << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h << '\t' << tag
           << '\t' << sub << '\t' << p << '\n'
           << std::dec;
    }
    std::printf("合并 %zu 个来源文件 -> %s，共 %zu 个节点定名\n", ins.size(), outTsv.c_str(),
                table.size());
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
        "  qtsvfs codecs                          已接入的压缩库\n"
        "  qtsvfs hash <path>...                  计算节点哈希 (VFS_CalcHashCode64Raw)\n"
        "  qtsvfs kdb info <file.db>              外层 QtskDB 头\n"
        "  qtsvfs kdb records <file.db> [--max=N] [--hist]   枚举 B+树记录\n"
        "  qtsvfs gindex <GlobalIndex.data> [--names=tsv]\n"
        "  qtsvfs tree <packageDir> [--depth=N]      用官方 node-index 还原真实文件树\n"
        "  qtsvfs nodes <file.db> [--max=N]          解码 FileNode 与块表\n"
        "  qtsvfs layout <packageDir> --names=<名单> [--depth=N]\n"
        "                                      用名字表还原真实目录树（未定名标 [nameless]）\n"
        "  qtsvfs export <packageDir> --names=<名单> --out=<目录> [--limit=N]\n"
        "                                      按真实路径解包落盘（路径先规整，防 ../ 逃逸）\n"
        "  qtsvfs scan <packageDir>...|--packages=<root> [--max=N] [--maxsize=N]\n"
        "                [--bare --gindex=<GlobalIndexPrime.data>] [--out=tsv] [--all] [--trace]\n"
        "                                      批量解块，找自声明并用哈希自校验；--bare 找裸资源路径\n"
        "  qtsvfs mergenames <tsv>... --out=<名字表>\n"
        "                                      按 real>object>named 优先级合并 scan 产出并同名消歧\n"
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
    if (sub == "export") {
        const auto files = positional({});
        if (files.empty()) {
            std::fprintf(stderr, "export 需要包目录参数\n");
            return 1;
        }
        std::string names, outDir;
        for (const auto& t : rest) {
            if (t.rfind("--names=", 0) == 0) {
                names = t.substr(8);
            } else if (t.rfind("--out=", 0) == 0) {
                outDir = t.substr(6);
            }
        }
        if (names.empty() || outDir.empty()) {
            std::fprintf(stderr, "export 需要 --names=<名单> 与 --out=<目录>\n");
            return 1;
        }
        std::uint64_t limit = 100000;
        optValue("--limit", limit, 100000);
        const bool nameless = std::any_of(rest.begin(), rest.end(),
                                          [](const std::string& t) { return t == "--nameless"; });
        return cmdExport(files[0], names, outDir, limit, nameless);
    }
    if (sub == "keys") {
        std::string outTsv, pkgRoot;
        for (const auto& t : rest) {
            if (t.rfind("--out=", 0) == 0) {
                outTsv = t.substr(6);
            } else if (t.rfind("--packages=", 0) == 0) {
                pkgRoot = t.substr(11);
            }
        }
        auto dirs = positional({"keys"});
        if (!pkgRoot.empty()) {
            for (const auto& e : std::filesystem::directory_iterator(utf8ToPath(pkgRoot))) {
                if (e.is_directory()) {
                    dirs.push_back(e.path());
                }
            }
            std::sort(dirs.begin(), dirs.end());
        }
        if (dirs.empty() || outTsv.empty()) {
            std::fprintf(stderr, "keys 需要 <包目录> 或 --packages=<root>，并给 --out=<tsv>\n");
            return 1;
        }
        return cmdKeys(dirs, outTsv);
    }
    if (sub == "layout") {
        const auto files = positional({});
        if (files.empty()) {
            std::fprintf(stderr, "layout 需要包目录参数，如 packages/101\n");
            return 1;
        }
        std::string names, outTsv;
        for (const auto& t : rest) {
            if (t.rfind("--names=", 0) == 0) {
                names = t.substr(8);
            } else if (t.rfind("--out=", 0) == 0) {
                outTsv = t.substr(6);
            }
        }
        if (names.empty()) {
            std::fprintf(stderr, "layout 需要 --names=<hash→path 名单>\n");
            return 1;
        }
        std::uint64_t depth = 3;
        optValue("--depth", depth, 3);
        return cmdLayout(files[0], names, depth, outTsv);
    }
    if (sub == "scan") {
        auto files = positional({});
        std::uint64_t max = 300, maxsize = 64ull * 1024 * 1024;
        if (!optValue("--max", max, 300) || !optValue("--maxsize", maxsize, 64ull * 1024 * 1024)) {
            std::fprintf(stderr, "选项解析失败\n");
            return 1;
        }
        std::string outTsv, gindexPath, pkgRoot, prefixArg, keysetArg, missingTsv, knownArg;
        bool showAll = false, trace = false, bare = false;
        for (const auto& t : rest) {
            if (t.rfind("--out=", 0) == 0) {
                outTsv = t.substr(6);
            } else if (t.rfind("--known=", 0) == 0) {
                knownArg = t.substr(8);
            } else if (t.rfind("--missing=", 0) == 0) {
                missingTsv = t.substr(10);
            } else if (t.rfind("--gindex=", 0) == 0) {
                gindexPath = t.substr(9);
            } else if (t.rfind("--packages=", 0) == 0) {
                pkgRoot = t.substr(11);
            } else if (t.rfind("--prefix=", 0) == 0) {
                prefixArg = t.substr(9);
            } else if (t.rfind("--keyset=", 0) == 0) {
                keysetArg = t.substr(9);
            } else if (t == "--all") {
                showAll = true;
            } else if (t == "--trace") {
                trace = true;
            } else if (t == "--bare") {
                bare = true;
            }
        }
        // 默认挂载根来自实测：官方白名单树的两个根 + harvest 里见到的两个根。
        std::vector<std::string> prefixes = {"", "rawassets/domestic/", "rawassets/shared/",
                                             "unity_buildin_payload/"};
        if (!prefixArg.empty()) {
            prefixes.clear();
            prefixes.push_back("");
            std::size_t pos = 0;
            while (pos < prefixArg.size()) {
                const std::size_t comma = prefixArg.find(',', pos);
                std::string p = prefixArg.substr(
                    pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!p.empty()) {
                    if (p.front() == '/') {
                        p.erase(p.begin());
                    }
                    if (p.back() != '/') {
                        p += '/';
                    }
                    prefixes.push_back(std::move(p));
                }
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
        }
        // 全库扫描时 1748 个目录参数会超 Windows 命令行上限，自己枚举。
        if (!pkgRoot.empty()) {
            for (const auto& e : std::filesystem::directory_iterator(utf8ToPath(pkgRoot))) {
                if (e.is_directory()) {
                    files.push_back(e.path());
                }
            }
            std::sort(files.begin(), files.end());
        }
        if (files.empty()) {
            std::fprintf(stderr, "scan 需要包目录参数或 --packages=<目录>\n");
            return 1;
        }
        std::unique_ptr<qtsvfs::GlobalIndex> gi;
        std::unordered_set<std::uint64_t> keyset;
        if (!keysetArg.empty()) {
            if (!loadKeyset(keysetArg, keyset)) {
                std::fprintf(stderr, "节点键集合读不到: %s\n", keysetArg.c_str());
                return 2;
            }
            std::printf("节点键集合: %zu 条\n", keyset.size());
        }
        if (bare) {
            if (gindexPath.empty()) {
                std::fprintf(stderr, "--bare 需要 --gindex=<GlobalIndexPrime.data> 做命中率判定\n");
                return 1;
            }
            gi = std::make_unique<qtsvfs::GlobalIndex>();
            std::string gerr;
            if (!gi->load(utf8ToPath(gindexPath), gerr)) {
                std::fprintf(stderr, "GlobalIndex 加载失败: %s\n", gerr.c_str());
                return 2;
            }
            std::printf("GlobalIndex: %s 文件项=%zu 噪声底约 %.3f%%\n", gindexPath.c_str(),
                        gi->fileCount(), 100.0 * static_cast<double>(gi->fileCount()) / 4294967296.0);
        }
        std::size_t done = 0, bad = 0;
        qtsvfs::NameTable known;
        if (!knownArg.empty()) {
            std::string kerr;
            if (!qtsvfs::loadNameTable(utf8ToPath(knownArg), known, kerr)) {
                std::fprintf(stderr, "%s\n", kerr.c_str());
                return 2;
            }
            std::printf("已定名集合: %zu 条（用于未定名原因分桶）\n", known.size());
        }
        std::ofstream missingOs;
        std::ostream* missing = nullptr;
        if (!missingTsv.empty()) {
            missingOs.open(utf8ToPath(missingTsv), std::ios::binary | std::ios::trunc);
            if (!missingOs) {
                std::fprintf(stderr, "写不出 %s\n", missingTsv.c_str());
                return 3;
            }
            missing = &missingOs;
        }
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (cmdScan(files[i], max, maxsize, outTsv, done > 0, showAll, trace, bare, gi.get(),
                prefixes, keyset.empty() ? nullptr : &keyset, missing,
                known.empty() ? nullptr : &known) !=
                0) {
                std::fprintf(stderr, "跳过 %s\n", wideToUtf8(files[i].native()).c_str());
                ++bad;
                continue;
            }
            ++done;
        }
        std::printf("扫描完成 %zu 个包，跳过 %zu 个（非包目录或打不开）\n", done, bad);
        return 0;
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
        return cmdNodes(files[0], max);
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
