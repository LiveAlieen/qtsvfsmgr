#include "qtsvfs/Package.h"

#include <algorithm>
#include <cstring>

#include "qtsvfs/codec/Codec.h"

namespace qtsvfs {
namespace {
std::uint32_t le32At(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
}  // namespace

bool Package::open(const std::filesystem::path& dir, std::string& err) {
    dir_ = dir;
    const std::filesystem::path meta = dir / (dir.filename().stem().wstring() + L".db");
    if (!std::filesystem::exists(meta)) {
        err = "找不到元数据卷: " + meta.string();
        return false;
    }
    metaReader_ = std::make_unique<FileReader>();
    if (!metaReader_->open(meta, err)) {
        return false;
    }
    if (!meta_.open(*metaReader_, err)) {
        err += " (" + meta.string() + ")";
        return false;
    }
    const std::wstring prefix = dir.filename().stem().wstring() + L"_";
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::filesystem::path p = e.path();
        if (!p.has_extension() || p.extension() != L".db") {
            continue;
        }
        const std::wstring stem = p.stem().wstring();
        if (stem.size() <= prefix.size() || stem.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        bool digits = true;
        for (std::size_t i = prefix.size(); i < stem.size(); ++i) {
            if (stem[i] < L'0' || stem[i] > L'9') {
                digits = false;
                break;
            }
        }
        if (!digits) {
            continue;
        }
        auto v = std::make_unique<Volume>();
        v->path = p;
        volumes_.push_back(std::move(v));
    }
    std::sort(volumes_.begin(), volumes_.end(),
              [](const auto& a, const auto& b) { return a->path < b->path; });
    return true;
}

bool Package::loadNodes(std::string& err) {
    if (!nodes_.empty()) {
        return true;
    }
    std::string local;
    meta_.forEachRecord(
        [&](const KdbRecord& r) {
            FileNode node;
            if (!node.parse(r.value.data(), r.value.size())) {
                return true;
            }
            nodes_[node.hash] = std::move(node);
            return true;
        },
        local);
    if (!local.empty()) {
        err = local;
    }
    return !nodes_.empty();
}

bool Package::indexVolume(Volume& v, std::string& err) {
    if (v.indexed) {
        return true;
    }
    v.reader = std::make_unique<FileReader>();
    if (!v.reader->open(v.path, err)) {
        return false;
    }
    if (!v.db.open(*v.reader, err)) {
        err += " (" + v.path.string() + ")";
        return false;
    }
    std::string local;
    v.db.forEachRecord(
        [&](const KdbRecord& r) {
            if (r.key.size() < 16) {
                return true;
            }
            BlockRef ref;
            std::memcpy(&ref.block, r.key.data() + 8, 4);
            std::memcpy(&ref.page, r.key.data() + 12, 4);
            ref.offset = r.offset;
            if (r.value.size() >= 4) {
                std::memcpy(&ref.declaredSize, r.value.data(), 4);
            }
            const std::uint64_t hash = static_cast<std::uint64_t>(le32At(r.key.data())) |
                                       (static_cast<std::uint64_t>(le32At(r.key.data() + 4)) << 32);
            v.byHash[hash].push_back(ref);
            return true;
        },
        local);
    v.indexed = true;
    if (!local.empty()) {
        err = local;
    }
    return true;
}

bool Package::readBlob(const FileNode& node, std::vector<std::uint8_t>& out, std::string& err) {
    const std::uint64_t hash = node.hash;
    std::vector<std::pair<Volume*, BlockRef>> all;
    for (auto& v : volumes_) {
        std::string ignore;
        if (!indexVolume(*v, ignore) || v->byHash.find(hash) == v->byHash.end()) {
            continue;
        }
        for (const auto& ref : v->byHash[hash]) {
            all.emplace_back(v.get(), ref);
        }
    }
    if (all.empty()) {
        err = "没有数据卷含该节点的块";
        return false;
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
        if (a.second.block != b.second.block) {
            return a.second.block < b.second.block;
        }
        if (a.second.page != b.second.page) {
            return a.second.page < b.second.page;
        }
        return a.second.offset < b.second.offset;
    });
    // 同一个 (block,page) 键可能留着多份历史页记录（实测包 117 节点 C6AABBB3BC88C681：
    // page1 有两份，声明 14059 与 14051，而节点总长 30443 = 16384 + 14059）。
    // 用块表来定性：非尾页应等于 blockSize，尾页等于 节点长度 - 已拼长度。
    struct Group {
        Volume* vol;
        BlockRef ref;
        std::vector<BlockRef> candidates;
    };
    std::vector<Group> groups;
    for (const auto& [vol, ref] : all) {
        if (!groups.empty() && groups.back().ref.block == ref.block &&
            groups.back().ref.page == ref.page) {
            groups.back().candidates.push_back(ref);
            continue;
        }
        Group g;
        g.vol = vol;
        g.ref = ref;
        g.candidates.push_back(ref);
        groups.push_back(std::move(g));
    }
    std::vector<std::pair<Volume*, BlockRef>> picked;
    picked.reserve(groups.size());
    std::uint64_t acc = 0;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        Group& g = groups[gi];
        const std::uint64_t pageSize = g.ref.block < node.blocks.size() ? node.blocks[g.ref.block].blockSize
                                                                       : 0;
        const bool last = gi + 1 == groups.size();
        const std::uint64_t expect =
            last ? (node.size > acc ? node.size - acc : 0) : pageSize;
        const BlockRef* chosen = &g.candidates.front();
        if (g.candidates.size() > 1) {
            for (const auto& c : g.candidates) {
                if (c.declaredSize == expect) {
                    chosen = &c;
                    break;
                }
            }
        }
        picked.emplace_back(g.vol, *chosen);
        acc += chosen->declaredSize;
    }

    out.clear();
    KdbRecord rec;
    for (const auto& [vol, ref] : picked) {
        std::string readErr;
        if (!vol->db.readRecord(ref.offset, rec, readErr)) {
            err = "块记录读取失败 @0x" + std::to_string(ref.offset) + ": " + readErr;
            return false;
        }
        if (rec.value.size() < 4) {
            err = "块值太短";
            return false;
        }
        if (ref.block >= node.blocks.size()) {
            err = "块号超出块表: " + std::to_string(ref.block);
            return false;
        }
        const std::uint8_t method = static_cast<std::uint8_t>(node.blocks[ref.block].packed & 0xFFu);
        const std::uint8_t* src = rec.value.data() + 4;
        const std::size_t srcLen = rec.value.size() - 4;
        const std::size_t cap = ref.declaredSize ? ref.declaredSize : srcLen * 8 + 1024;
        // ooz 的末尾 quantum 会按字长多写几十字节（实测包 116 的 872 字节块踩坏堆），
        // 所以物理缓冲留 SAFE_SPACE，但传给解码器的容量仍是页里声明的长度，
        // LZ4_decompress_fast 依赖这个长度决定停止位置，不能放大。
        constexpr std::size_t kSlack = 64;
        const std::size_t at = out.size();
        out.resize(at + cap + kSlack);
        std::string decErr;
        const std::size_t n = decompressByMethod(method, src, srcLen, out.data() + at, cap, decErr);
        if (n == 0) {
            err = "解压失败(block=" + std::to_string(ref.block) + " page=" +
                  std::to_string(ref.page) + " method=" + std::to_string(method) + "): " + decErr;
            out.resize(at);
            return false;
        }
        out.resize(at + std::min(n, cap));
    }
    return true;
}

}  // namespace qtsvfs
