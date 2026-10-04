#include "qtsvfs/Names.h"

#include <cctype>
#include <fstream>

namespace qtsvfs {

bool loadNameTable(const std::filesystem::path& file, NameTable& out, std::string& err) {
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
        std::string path;
        const std::size_t t2 = line.find('\t', 17);
        if (t2 == std::string::npos) {
            path = line.substr(17);
        } else if (line.compare(17, t2 - 17, "named") == 0) {
            const std::size_t t3 = line.find('\t', t2 + 1);
            path = t3 == std::string::npos ? "" : line.substr(t3 + 1);
        } else {
            continue;
        }
        if (path.empty()) {
            continue;
        }
        const std::uint64_t h = std::strtoull(line.substr(0, 16).c_str(), nullptr, 16);
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

}  // namespace qtsvfs
