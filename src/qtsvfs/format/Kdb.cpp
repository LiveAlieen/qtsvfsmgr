#include "qtsvfs/format/Kdb.h"

#include <cstdio>
#include <unordered_set>

namespace qtsvfs {
namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

std::uint32_t le16(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
}

std::uint32_t align4(std::uint32_t v) { return (v + 3u) & ~3u; }

// 页内固定位置：槽键数组在 +0x00，子页/记录偏移数组在 +0x7F8，页尾 16 字节元数据在 +0xFF0。
constexpr std::uint32_t kSlotKeys = 0x000;
constexpr std::uint32_t kSlotRefs = 0x7F8;
constexpr std::uint32_t kTrailer = 0xFF0;
constexpr std::uint32_t kRecordHeaderSize = 0x1C;

std::string toHexU32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

}  // namespace

bool KdbFile::open(FileReader& reader, std::string& err) {
    if (reader.size() < kKdbPageSize) {
        err = "文件太小，不是 QtskDB";
        return false;
    }
    auto head = reader.read(0, 80, err);
    if (!err.empty()) {
        return false;
    }
    header_.magic = le64(head.data());
    if (header_.magic != kKdbMagic) {
        err = "魔数不符: 0x" + std::to_string(header_.magic);
        return false;
    }
    for (int i = 0; i < 18; ++i) {
        const std::uint32_t v = le32(head.data() + 8 + i * 4);
        switch (i) {
            case 0: header_.usedSize = v; break;
            case 1: header_.commitSize = v; break;
            case 2: header_.rootPage = v; break;
            case 3: header_.rootPageAlias = v; break;
            case 4: header_.chainA = v; break;
            case 5: header_.chainB = v; break;
            case 6: header_.tocOffset = v; break;
            case 7: header_.tocLength = v; break;
            case 8: header_.tocCount = v; break;
            case 9: header_.tocSerial = v; break;
            default: header_.alloc[i - 10] = v; break;
        }
    }
    if (header_.rootPage == kKdbNull || header_.rootPage + kKdbPageSize > reader.size()) {
        err = "根页偏移越界";
        return false;
    }
    reader_ = &reader;
    return true;
}

bool KdbFile::readPage(std::uint32_t offset, std::vector<std::uint8_t>& out, std::string& err) {
    if (static_cast<std::uint64_t>(offset) + kKdbPageSize > reader_->size()) {
        err = "页偏移越界: " + std::to_string(offset);
        return false;
    }
    out = reader_->read(offset, kKdbPageSize, err);
    return err.empty();
}

bool KdbFile::readRecord(std::uint64_t offset, KdbRecord& out, std::string& err) {
    auto prefix = reader_->read(offset, kRecordHeaderSize, err);
    if (!err.empty()) {
        return false;
    }
    const std::uint32_t self = le32(prefix.data());
    const std::uint32_t total = le32(prefix.data() + 8);
    out.offset = offset;
    out.hash1 = le32(prefix.data() + 0xC);
    out.hash2 = le32(prefix.data() + 0x10);
    out.keyLen = le32(prefix.data() + 0x14);
    out.valueLen = le32(prefix.data() + 0x18);
    out.totalLen = total;
    if (self != static_cast<std::uint32_t>(offset) || total < kRecordHeaderSize ||
        total > kKdbPageSize * 256) {
        err = "记录头不自洽 @0x" + std::to_string(offset);
        return false;
    }
    auto body = reader_->read(offset + kRecordHeaderSize, total - kRecordHeaderSize, err);
    if (!err.empty()) {
        return false;
    }
    const std::uint32_t vlen = align4(out.valueLen);
    const std::uint32_t klen = align4(out.keyLen);
    if (vlen + klen > body.size()) {
        err = "记录载荷长度越界 @0x" + std::to_string(offset);
        return false;
    }
    // 载荷顺序是先 value 后 key（各自补齐到 4 字节）。
    out.value.assign(body.begin(), body.begin() + out.valueLen);
    out.key.assign(body.begin() + vlen, body.begin() + vlen + out.keyLen);
    return true;
}

std::uint64_t KdbFile::forEachRecord(const std::function<bool(const KdbRecord&)>& visit,
                                     std::string& err) {
    warnings_.clear();
    std::uint64_t seen = 0;
    std::vector<std::uint32_t> stack;
    std::unordered_set<std::uint32_t> visited;
    std::vector<std::uint8_t> page;

    stack.push_back(header_.rootPage);
    while (!stack.empty()) {
        const std::uint32_t off = stack.back();
        stack.pop_back();
        if (off == kKdbNull || !visited.insert(off).second) {
            continue;
        }
        if (!readPage(off, page, err)) {
            warnings_.push_back("跳过不可读页 0x" + toHexU32(off) + ": " + err);
            err.clear();
            continue;
        }
        // 页尾 16 字节：self / prev / next（这三个是区段链，不表示兄弟）/ 槽数 / 标志。
        const std::uint32_t count = le16(page.data() + kTrailer + 12);
        const std::uint16_t flags = static_cast<std::uint16_t>(le16(page.data() + kTrailer + 14));
        if (le32(page.data() + kTrailer) != off || count == 0 || count > kKdbMaxSlots) {
            warnings_.push_back("跳过异常页 0x" + toHexU32(off) + " (cnt=" + std::to_string(count) +
                                 ")");
            continue;
        }
        if (flags & 1) {
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t recOff = le32(page.data() + kSlotRefs + i * 4);
                if (recOff == kKdbNull) {
                    continue;
                }
                KdbRecord rec;
                if (!readRecord(recOff, rec, err)) {
                    warnings_.push_back("跳过坏记录 0x" + toHexU32(recOff) + " (页 0x" +
                                        toHexU32(off) + " 槽 " + std::to_string(i) + "): " + err);
                    err.clear();
                    continue;
                }
                ++seen;
                if (!visit(rec)) {
                    return seen;
                }
            }
        } else {
            // 内部节点：子页偏移数组，倒序入栈以得到按 key 升序的前向遍历。
            for (std::uint32_t i = count; i > 0; --i) {
                stack.push_back(le32(page.data() + kSlotRefs + (i - 1) * 4));
            }
        }
    }
    return seen;
}

}  // namespace qtsvfs
