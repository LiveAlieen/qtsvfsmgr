#include "qtsvfs/Names.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_set>
#include <vector>

namespace qtsvfs {

std::string safeRelative(const std::string& path) {
    std::string res;
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == '/' || path[i] == '\\')) {
            ++i;
        }
        std::size_t j = i;
        while (j < path.size() && path[j] != '/' && path[j] != '\\') {
            ++j;
        }
        std::string seg = path.substr(i, j - i);
        i = j;
        if (seg.empty() || seg == "." || seg == "..") {
            continue;
        }
        for (char& c : seg) {
            if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                static_cast<unsigned char>(c) < 32) {
                c = '_';
            }
        }
        if (seg.size() > 96) {
            seg = seg.substr(0, 96);
        }
        if (!res.empty()) {
            res += '/';
        }
        res += seg;
    }
    return res;
}

void makeUnique(NameTable& names) {
    std::vector<std::uint64_t> keys;
    keys.reserve(names.size());
    for (const auto& [h, p] : names) {
        keys.push_back(h);
    }
    std::sort(keys.begin(), keys.end());

    auto lower = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    std::unordered_set<std::string> used;
    used.reserve(names.size() * 2);
    for (const std::uint64_t h : keys) {
        const auto it = names.find(h);
        if (used.insert(lower(it->second)).second) {
            continue;
        }
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "~%08llX",
                      static_cast<unsigned long long>(h & 0xFFFFFFFFu));
        const std::size_t slash = it->second.rfind('/');
        const std::size_t head = slash == std::string::npos ? 0 : slash + 1;
        const std::size_t dot = it->second.rfind('.');
        const std::size_t at = (dot != std::string::npos && dot > head) ? dot : it->second.size();
        it->second.insert(at, suffix);
        used.insert(lower(it->second));
    }
}

}  // namespace qtsvfs
