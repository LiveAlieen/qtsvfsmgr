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

// 资源真名：不含 '/'、不含 '.'（带点的是版本号/文件名那类字段）、必须有字母。
// 首字符是 '_' 的排除掉：那是着色器属性/关键字（_HSVConversionMatrix_B、_SGAME_POINT_LIGHT_ON），
// 实测包 8 里这类串会以 2.8 万次的量级重复出现，当成文件名等于什么都没定名。
bool nameLike(const std::string& s) {
    if (s.size() < 3 || s.size() > 64 || s[0] == '_' || s.find('.') != std::string::npos) {
        return false;
    }
    bool letter = false;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                        (u >= '0' && u <= '9') || c == '_' || c == '-' || c == ' ' || c == '(' ||
                        c == ')';
        if (!ok) {
            return false;
        }
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')) {
            letter = true;
        }
    }
    return letter;
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
    if (names.empty() || pathCount > 1) {
        return false;  // 没名字，或者本体是聚合 bundle（一个文件装多个资源）
    }
    const bool single = names.size() == 1 || (names.size() == 2 && *names[0] == *names[1]);
    const std::string* pick = names[0];
    if (!single) {
        // 子对象名（source、Bone007、_TINTCOLOR_ON、Dead）普遍比资源名短且少分段，
        // 按 长度+2×下划线 取胜者，实测包 8 会选 EF_zxq_lobby_51703_149 而不是 _TINTCOLOR_ON。
        auto score = [](const std::string& s) {
            std::size_t us = 0;
            for (char c : s) {
                if (c == '_') {
                    ++us;
                }
            }
            return s.size() + 2 * us;
        };
        std::size_t best = score(**names.begin());
        for (const std::string* p : names) {
            if (score(*p) > best) {
                best = score(*p);
                pick = p;
            }
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

}  // namespace qtsvfs
