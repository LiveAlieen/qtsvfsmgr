#include "qtsvfs/Catalog.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace qtsvfs {
namespace {

constexpr std::size_t kMaxField = 80;
constexpr std::size_t kNameWindow = 320;

bool isPathChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
           c == '_' || c == '-' || c == '.' || c == '/' || c == '+';
}

bool printable(const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] < 0x20 || p[i] > 0x7e) {
            return false;
        }
    }
    return n > 0;
}

// 声明路径：含 '/'、末尾有扩展名、字符集受限。
bool pathLike(const std::string& s) {
    if (s.size() < 8 || s.find('/') == std::string::npos) {
        return false;
    }
    const std::size_t dot = s.rfind('.');
    if (dot == std::string::npos || dot + 1 >= s.size() || s.size() - dot - 1 > 8) {
        return false;
    }
    for (char c : s) {
        if (!isPathChar(c)) {
            return false;
        }
    }
    return true;
}

// 资源真名：不含 '/'、不含 '.'（带点的是版本号/文件名那类字段）。
// 首字符是 '_' 的排除掉：那是着色器属性/关键字（_HSVConversionMatrix_B、_SGAME_POINT_LIGHT_ON），
// 实测包 8 里这类串会以 2.8 万次的量级重复出现，当成文件名等于什么都没定名。
// 允许 [ ] 与 @ —— 统计过名字位上真实出现过的非字母数字字符：`_` 7004、`@` 281
// （PJD_M_03JungleGrassB_03@@_low）、`[`/`]` 各 200（Unity 重名后缀 `名字 [1]`）、
// `-` 92、空格 36。长尾那些 `` ` ! < ? > = ; # % ~ + : `` 都只出现 1~2 次且例子是
// `!I<`、`K~Z` 这类二进制噪声，不收。
// 纯数字也算名字（包 305000000906 那批资源的名字槽里放的就是资源号 100502/190360），
// 但打分时带字母的必须压过纯数字，见 parseSelfName。
bool nameLike(const std::string& s) {
    if (s.size() < 3 || s.size() > 64 || s[0] == '_' || s.find('.') != std::string::npos) {
        return false;
    }
    bool letter = false;
    bool digit = false;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                        (u >= '0' && u <= '9') || c == '_' || c == '-' || c == ' ' || c == '(' ||
                        c == ')' || c == '[' || c == ']' || c == '@';
        if (!ok) {
            return false;
        }
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')) {
            letter = true;
        }
        if (u >= '0' && u <= '9') {
            digit = true;
        }
    }
    return letter || (digit && s.size() >= 4);
}

// bundle 内部资源路径：形如 prefab_skill_effects/tongyong_effects/…/jidibaozha_normal_01、
// common/room.gl。它们不指向 VFS 节点（哈希不上），但确实是这个资源自己声明的身份。
// 排除末段是长十六进制串的（assets/fd/fd090a….resS 那是兄弟流的路径，拿来当自己的名字会撞车）。
bool internalPathLike(const std::string& s) {
    if (s.size() < 8 || s.size() > 120 || s.find('/') == std::string::npos) {
        return false;
    }
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                        (u >= '0' && u <= '9') || c == '_' || c == '-' || c == '.' || c == '/' ||
                        c == ' ';
        if (!ok) {
            return false;
        }
    }
    const std::size_t slash = s.rfind('/');
    const std::string last = s.substr(slash + 1);
    std::size_t hex = 0;
    for (char c : last) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool isHex = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
        if (isHex) {
            ++hex;
        }
    }
    return hex < 16;
}

// 只有「引擎序列化资源文件」才允许拿体内字段当文件名：这类文件的头 64 字节里带
// 引擎版本串（实测包 101 的 1240 个无名节点 1197 个如此，形如 2022.3.5f1）。
// 少了这道闸，JS 源码那种「体内唯一一个短标识符」也会被当成名字（require、__esModule）。
bool serializedHeader(const std::uint8_t* data, std::size_t size) {
    const std::size_t n = std::min<std::size_t>(size, 64);
    for (std::size_t i = 0; i + 6 < n; ++i) {
        if (data[i] < '0' || data[i] > '9' || data[i + 1] < '0' || data[i + 1] > '9' ||
            data[i + 2] < '0' || data[i + 2] > '9' || data[i + 3] < '0' || data[i + 3] > '9' ||
            data[i + 4] != '.') {
            continue;
        }
        std::size_t k = i + 5;
        int digits = 0;
        int dots = 1;
        while (k < n) {
            const unsigned char c = data[k];
            if (c >= '0' && c <= '9') {
                ++digits;
                ++k;
            } else if (c == '.' && dots < 2) {
                ++dots;
                ++k;
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                ++k;
            } else {
                break;
            }
        }
        if (digits >= 2 && dots >= 2) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::pair<std::size_t, std::string>> collectFields(const std::uint8_t* data,
                                                               std::size_t size) {
    std::vector<std::pair<std::size_t, std::string>> fields;  // (串起始偏移, 值)
    for (std::size_t i = 0; i + 4 < size; ++i) {
        // 定长头是小端 u32，长度都落在 3..80，后三字节必为 0，先把它当筛子。
        if (data[i] < 3 || data[i] > kMaxField || data[i + 1] || data[i + 2] || data[i + 3]) {
            continue;
        }
        const std::size_t len = data[i];
        if (i + 4 + len > size || !printable(data + i + 4, len)) {
            continue;
        }
        fields.emplace_back(i + 4, std::string(reinterpret_cast<const char*>(data + i + 4), len));
    }
    return fields;
}

std::vector<CatalogEntry> parseCatalog(const std::uint8_t* data, std::size_t size) {
    const auto fields = collectFields(data, size);
    std::vector<CatalogEntry> out;
    for (std::size_t k = 0; k < fields.size(); ++k) {
        const std::string& path = fields[k].second;
        if (!pathLike(path)) {
            continue;
        }
        const std::size_t poff = fields[k].first;
        const std::size_t lo = poff > kNameWindow ? poff - kNameWindow : 0;
        std::string name;
        for (std::size_t j = k; j-- > 0;) {
            if (fields[j].first <= lo) {
                break;
            }
            if (nameLike(fields[j].second)) {
                name = fields[j].second;
                break;
            }
        }
        if (name.empty()) {
            continue;
        }
        CatalogEntry e;
        e.name = std::move(name);
        e.path = path;
        e.ext = path.substr(path.rfind('.') + 1);
        for (char& c : e.ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        out.push_back(std::move(e));
    }
    return out;
}

bool parseSelfName(const std::uint8_t* data, std::size_t size, SelfName& out) {
    if (!serializedHeader(data, size)) {
        return false;
    }
    const auto fields = collectFields(data, size);
    std::vector<const std::string*> names;
    std::size_t pathCount = 0;
    const std::string* dirSrc = nullptr;
    for (const auto& [off, val] : fields) {
        if (pathLike(val)) {
            if (!dirSrc) {
                dirSrc = &val;
            }
            ++pathCount;
            continue;
        }
        if (nameLike(val)) {
            names.push_back(&val);
        }
    }
    if (pathCount > 1) {
        return false;  // 本体是聚合 bundle（一个文件装多个资源），取哪个名字都不对
    }
    if (names.empty()) {
        // 没有名字字段时，退一步用体内声明的 bundle 内部路径当名字（例：common/room.gl、
        // prefab_skill_effects/tongyong_effects/…/jidibaozha_normal_01）。
        // 这类串哈希不到节点键，所以它是「资源自己说的身份」而不是「VFS 路径」，
        // 来源单独标成 objpath，不跟前几层混。
        const std::string* bestPath = nullptr;
        for (const auto& [off, val] : fields) {
            if (pathLike(val)) {
                continue;
            }
            if (internalPathLike(val) && (!bestPath || val.size() > bestPath->size())) {
                bestPath = &val;
            }
        }
        if (!bestPath) {
            return false;
        }
        out.confident = false;
        out.fromPath = true;
        out.name = *bestPath;
        return true;
    }
    const bool single = names.size() == 1 || (names.size() == 2 && *names[0] == *names[1]);
    const std::string* pick = names[0];
    if (!single) {
        // 子对象名（source、Bone007、_TINTCOLOR_ON、Dead）普遍比资源名短且少分段，
        // 按 长度+2×下划线 取胜者，实测包 8 会选 EF_zxq_lobby_51703_149 而不是 _TINTCOLOR_ON。
        // 带字母的加固定权重，保证纯资源号（100502）只在没有别的候选时才用。
        auto score = [](const std::string& s) {
            std::size_t us = 0;
            bool letter = false;
            for (char c : s) {
                if (c == '_') {
                    ++us;
                }
                const unsigned char u = static_cast<unsigned char>(c);
                if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')) {
                    letter = true;
                }
            }
            return s.size() + 2 * us + (letter ? 1000u : 0u);
        };
        std::size_t best = score(*names.front());
        for (const std::string* p : names) {
            if (score(*p) > best) {
                best = score(*p);
                pick = p;
            }
        }
        // 择优到 5 个字符以下说明体内根本没有像样的名字，那是二进制里的巧合片段
        // （实测界面上出现过 D_、(vo、)dl 这种），宁可留哈希名也别造假名字。
        if (pick->size() < 6) {
            return false;
        }
    }
    out.confident = single;
    out.name = *pick;
    if (dirSrc) {
        const std::size_t slash = dirSrc->rfind('/');
        out.dir = slash == std::string::npos ? "" : dirSrc->substr(0, slash);
        for (char& c : out.dir) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return true;
}

CatalogStats measureCatalog(const std::uint8_t* data, std::size_t size) {
    CatalogStats st;
    st.serialized = serializedHeader(data, size);
    for (const auto& [off, val] : collectFields(data, size)) {
        if (pathLike(val)) {
            ++st.paths;
        } else if (nameLike(val)) {
            ++st.names;
        }
    }
    const std::size_t n = std::min<std::size_t>(size, 4);
    for (std::size_t i = 0; i < n; ++i) {
        st.magic += (data[i] >= 0x20 && data[i] <= 0x7e) ? static_cast<char>(data[i]) : '.';
    }
    return st;
}

}  // namespace qtsvfs
