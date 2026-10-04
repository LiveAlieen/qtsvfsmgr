#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "qtsvfs/format/Kdb.h"
#include "qtsvfs/format/QtsfNode.h"
#include "qtsvfs/io/FileReader.h"

namespace qtsvfs {

// packages/<id>/ 目录：一个元数据卷 <id>.db（放 FileNode）+ 若干数据卷 <id>_N.db（放压缩块）。
class Package {
public:
    bool open(const std::filesystem::path& dir, std::string& err);

    // 元数据卷里的 FileNode，按节点哈希索引。
    bool loadNodes(std::string& err);
    const std::unordered_map<std::uint64_t, FileNode>& nodes() const noexcept { return nodes_; }

    // 按节点块表从数据卷取块，用块里声明的 method id 解压后按 (block, page) 拼接。
    bool readBlob(const FileNode& node, std::vector<std::uint8_t>& out, std::string& err);

    std::size_t volumeCount() const noexcept { return volumes_.size(); }

private:
    struct BlockRef {
        std::uint32_t block = 0;
        std::uint32_t page = 0;
        std::uint64_t offset = 0;
        std::uint32_t declaredSize = 0;
    };
    struct Volume {
        std::filesystem::path path;
        std::unique_ptr<FileReader> reader;
        KdbFile db;
        std::unordered_map<std::uint64_t, std::vector<BlockRef>> byHash;
        bool indexed = false;
    };

    bool indexVolume(Volume& v, std::string& err);

    std::filesystem::path dir_;
    std::unique_ptr<FileReader> metaReader_;
    KdbFile meta_;
    std::unordered_map<std::uint64_t, FileNode> nodes_;
    std::vector<std::unique_ptr<Volume>> volumes_;
};

}  // namespace qtsvfs
