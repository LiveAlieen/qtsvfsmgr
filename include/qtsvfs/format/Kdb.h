#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "qtsvfs/io/FileReader.h"

namespace qtsvfs {

// QtskDB / BTreeDB 外层容器（packages/<id>/<id>.db 与 <id>_N.db）。
// 证据：libQtskDB.so.c 的 BTreeDB.cpp open/format 路径。
inline constexpr std::uint64_t kKdbMagic = 0x0403020102000001ull;
inline constexpr std::uint32_t kKdbPageSize = 0x1000;
inline constexpr std::uint32_t kKdbMaxSlots = 0x1FE;
inline constexpr std::uint64_t kKdbNull = 0xFFFFFFFFull;

struct KdbHeader {
    std::uint64_t magic = 0;
    std::uint32_t usedSize = 0;     // @8  分配高水位
    std::uint32_t commitSize = 0;   // @12 已提交大小（0x4000 粒度）
    std::uint32_t rootPage = 0;     // @16
    std::uint32_t rootPageAlias = 0;  // @20
    std::uint32_t chainA = 0;       // @24
    std::uint32_t chainB = 0;       // @28
    std::uint32_t tocOffset = 0;    // @32
    std::uint32_t tocLength = 0;    // @36
    std::uint32_t tocCount = 0;     // @40
    std::uint32_t tocSerial = 0;    // @44
    std::uint32_t alloc[8] = {};    // @48..@79 四组 (头,尾) 空闲链
};

struct KdbRecord {
    std::uint64_t offset = 0;
    std::uint32_t hash1 = 0;
    std::uint32_t hash2 = 0;
    std::uint32_t totalLen = 0;
    std::uint32_t keyLen = 0;
    std::uint32_t valueLen = 0;
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> value;
};

class KdbFile {
public:
    bool open(FileReader& reader, std::string& err);

    const KdbHeader& header() const noexcept { return header_; }
    std::uint64_t size() const noexcept { return reader_->size(); }

    // 深度优先遍历叶子页，按 (页内槽位顺序) 回调记录。返回访问到的记录数。
    // 深度遍历整棵 B+树。坏子页只记录警告并跳过，不中断整体遍历。
    std::uint64_t forEachRecord(const std::function<bool(const KdbRecord&)>& visit,
                               std::string& err);

    const std::vector<std::string>& warnings() const noexcept { return warnings_; }

    // 供上层按绝对偏移定点取记录（B+树下降后定位到具体槽位时用）。
    bool readRecord(std::uint64_t offset, KdbRecord& out, std::string& err);

    bool readPage(std::uint32_t offset, std::vector<std::uint8_t>& out, std::string& err);

private:
    FileReader* reader_ = nullptr;
    KdbHeader header_;
    std::vector<std::string> warnings_;
};

}  // namespace qtsvfs
