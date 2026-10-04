#include "qtsvfs/Names.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <unordered_set>
#include <vector>

namespace qtsvfs {

bool loadNameTable(const std::filesystem::path& file, NameTable& out, std::string& err,
                   SourceTable* sources) {
    std::ifstream in(file);
    if (!in) {
        err = "打不开名字表: " + file.string();
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 18 || line[16] != '\t') {
            continue;
        }
        const std::uint64_t h = std::strtoull(line.substr(0, 16).c_str(), nullptr, 16);
        std::string path, tag;
        const std::size_t t2 = line.find('\t', 17);
        if (t2 == std::string::npos) {
            path = line.substr(17);
        } else {
            tag = line.substr(17, t2 - 17);
            if (tag != "named" && tag != "real" && tag != "object") {
                continue;
            }
            const std::size_t t3 = line.find('\t', t2 + 1);
            if (t3 == std::string::npos) {
                continue;
            }
            path = line.substr(t3 + 1);
            if (sources) {
                const std::string sub = line.substr(t2 + 1, t3 - t2 - 1);
                sources->try_emplace(h, sub.empty() ? tag : tag + ":" + sub);
            }
        }
        if (path.empty()) {
            continue;
        }
        out.emplace(h, std::move(path));
    }
    if (out.empty()) {
        err = "名字表为空: " + file.string();
        return false;
    }
    return true;
}

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
