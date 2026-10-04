#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "qtsvfs/format/Kdb.h"
#include "qtsvfs/format/QtsfNode.h"
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
        "  qtsvfs nodes <file.db> [--max=N]          解码 FileNode 与块表\n"
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
