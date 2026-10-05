#include "qtsvfs/Alias.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtsvfs/Names.h"
#include "qtsvfs/format/PathHash.h"

namespace qtsvfs {
namespace {

bool pathChar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '[' || c == ']' || c == '@' || c == '-' ||
           c == '(' || c == ')' || c == '.' || c == '/' || c == ' ';
}

bool streamExtension(std::string_view s) {
    const std::size_t dot = s.rfind('.');
    if (dot == std::string_view::npos || s.size() - dot > 16) {
        return false;
    }
    std::string ext(s.substr(dot));
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".ress" || ext == ".resource";
}

std::string lowered(std::string_view s) {
    std::string o(s);
    for (char& c : o) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return o;
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Unity 把 m_StreamData.path 写成 [i32 长度][字节]，所以按长度前缀取串最稳：
// 直接找可打印连续段会被长度字段本身（比如 0x2F 正好是 '/'）粘上脏字节。
// 长度不超过 240 ⇒ 这个 u32 的高三字节必为 0，一次整数比较就能否掉绝大多数位置。
// 注意：校验不过只能挪一个字节继续，不能按 len 跨过——纹理/网格体里 8..240 的小整数
// 到处都是，一跨就把紧随其后的真引用整条跳掉（实测这样漏掉了 369/639 条）。
void findRefs(const std::uint8_t* buf, std::size_t size, std::vector<std::string>& out) {
    if (size < 12) {
        return;
    }
    for (std::size_t p = 0; p + 8 < size; ++p) {
        const std::uint32_t len = le32(buf + p);
        if (len < 8 || len > 240 || p + 4 + len > size) {
            continue;
        }
        const std::string_view s(reinterpret_cast<const char*>(buf + p + 4), len);
        if (s.find('/') == std::string_view::npos || s.find("..") != std::string_view::npos) {
            continue;
        }
        bool ok = true;
        for (char c : s) {
            if (!pathChar(static_cast<unsigned char>(c))) {
                ok = false;
                break;
            }
        }
        if (ok && streamExtension(s)) {
            out.emplace_back(s);
            p += 4 + len;  // 收下了才整条跨过，免得同一条被重复计数
        }
    }
}

enum Act { kLinked = 0, kAlready = 1, kStale = 2, kFailed = 3 };

std::string relUtf8(const std::filesystem::path& p) {
    const auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

AliasResult linkStreamAliases(const std::filesystem::path& root, const std::filesystem::path& manifest,
                              bool dryRun, int threads, const std::filesystem::path& refsPath,
                              const std::function<void(const AliasResult&)>& progress) {
    AliasResult res;
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        res.error = "manifest 读不到: " + manifest.string();
        return res;
    }
    std::unordered_map<std::uint64_t, std::string> byKey;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 20 || line.rfind("hash", 0) == 0) {
            continue;
        }
        const std::size_t t1 = line.find('\t');
        const std::size_t t2 = line.find('\t', t1 + 1);
        const std::size_t t3 = line.find('\t', t2 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) {
            continue;
        }
        const std::uint64_t key = std::strtoull(line.substr(0, t1).c_str(), nullptr, 16);
        const std::string rel = line.substr(t2 + 1, t3 == std::string::npos ? std::string::npos
                                                                           : t3 - t2 - 1);
        if (key && !rel.empty()) {
            byKey[key] = rel;  // 同键多行（重跑过）以最后一条为准
        }
    }

    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (e.is_regular_file()) {
            files.push_back(e.path());
        }
    }
    if (files.empty()) {
        res.error = "导出树里没有文件: " + root.string();
        return res;
    }
    if (threads <= 0) {
        threads = static_cast<int>(std::thread::hardware_concurrency());
    }
    threads = std::max(1, std::min(threads, 32));
    res.total = files.size();

    std::ofstream refsOut;
    if (!refsPath.empty()) {
        refsOut.open(refsPath, std::ios::binary | std::ios::trunc);
        if (!refsOut) {
            res.error = "边表写不出: " + refsPath.string();
            return res;
        }
        refsOut << "host\tstream\n";
    }

    // 读文件、找引用、建链接全在锁外做，只有计数和「这条引用已经处理过」的去重上锁，
    // 否则磁盘带宽就被一把锁卡住了。
    std::mutex mx;
    std::unordered_set<std::string> planned;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> tick{1};
    auto worker = [&] {
        std::vector<std::uint8_t> buf;
        std::vector<std::string> refs;
        std::vector<std::pair<Act, std::string>> acts;
        std::vector<std::string> edges;
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= files.size()) {
                return;
            }
            std::ifstream f(files[i], std::ios::binary | std::ios::ate);
            std::streamoff size = 0;
            if (f) {
                size = f.tellg();
            }
            if (size < 60) {
                std::lock_guard<std::mutex> lk(mx);
                ++res.scanned;
                continue;
            }
            f.seekg(0);
            buf.resize(static_cast<std::size_t>(size));
            if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
                std::lock_guard<std::mutex> lk(mx);
                ++res.failed;
                ++res.scanned;
                continue;
            }
            refs.clear();
            acts.clear();
            edges.clear();
            findRefs(buf.data(), buf.size(), refs);
            std::error_code rec;
            const std::string owner = relUtf8(std::filesystem::relative(files[i], root, rec));
            std::uint64_t nrefs = 0, nhit = 0;
            for (const auto& ref : refs) {
                ++nrefs;
                const auto it = byKey.find(calcHashCode64("/" + lowered(ref)));
                if (it == byKey.end()) {
                    continue;
                }
                ++nhit;
                if (refsOut) {
                    edges.push_back(owner + '\t' + safeRelative(ref));
                }
                const std::string what = ref + '\n' + it->second;
                const std::filesystem::path target = root / safeRelative(it->second);
                const std::filesystem::path alias = root / safeRelative(ref);
                std::error_code lec;
                if (std::filesystem::exists(alias, lec)) {
                    acts.emplace_back(kAlready, what);  // 名字表给的就是规范路径
                    continue;
                }
                if (!std::filesystem::exists(target, lec)) {
                    acts.emplace_back(kStale, what);  // 键命中但文件不在树上（在别的包或本机无字节）
                    continue;
                }
                if (dryRun) {
                    acts.emplace_back(kLinked, what);
                    continue;
                }
                std::error_code mec;
                std::filesystem::create_directories(alias.parent_path(), mec);
                std::filesystem::create_hard_link(target, alias, lec);
                if (lec) {
                    // 别的线程可能刚给同一个键补了同一个名字：那就当成已存在
                    std::error_code pec;
                    acts.emplace_back(std::filesystem::exists(alias, pec) ? kAlready : kFailed, what);
                } else {
                    acts.emplace_back(kLinked, what);
                }
            }
            std::lock_guard<std::mutex> lk(mx);
            res.refs += nrefs;
            res.hit += nhit;
            ++res.scanned;
            for (const auto& e : edges) {
                refsOut << e << '\n';
            }
            for (const auto& [kind, what] : acts) {
                if (!planned.insert(what).second) {
                    continue;  // 同一条引用被多个文件引用，只算一次
                }
                switch (kind) {
                    case kLinked: ++res.linked; break;
                    case kAlready: ++res.already; break;
                    case kStale: ++res.stale; break;
                    default: ++res.failed; break;
                }
            }
            if (progress && tick.fetch_add(1) % 50000 == 0) {
                progress(res);
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) {
        pool.emplace_back(worker);
    }
    worker();  // 主线程也干一份
    for (auto& t : pool) {
        t.join();
    }
    return res;
}

}  // namespace qtsvfs
