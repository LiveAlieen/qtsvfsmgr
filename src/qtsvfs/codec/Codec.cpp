#include "qtsvfs/codec/Codec.h"

#include <cstring>

extern "C" {
#include "lz4.h"
}
// 需要 ZSTD_decompressBegin / ZSTD_decompressBlock（experimental 区）
#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#endif
#include "zstd.h"
#include "ooz_wrapper.h"

namespace qtsvfs {

const char* methodName(std::uint8_t id) {
    switch (id) {
        case kMethodLz4: return "lz4";
        case kMethodLz4Hc: return "lz4hc";
        case kMethodOodle: return "oodle";
        case kMethodLz3: return "lz3";
        case kMethodZstd: return "zstd";
        default: return "未知 method";
    }
}

namespace {

// 无帧 zstd 裸块流：3 字节块头 = bit0 last | bit1-2 type | 高 21 位 size，
// type 0=raw 1=RLE 2=compressed。包 7 的 7426 个页没有一个是 zstd 帧，全是这种流。
std::size_t decodeZstdRawStream(const std::uint8_t* src, std::size_t srcLen, std::uint8_t* dst,
                                std::size_t dstCapacity, std::string& err) {
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (dctx == nullptr) {
        err = "zstd: 无法创建 DCtx";
        return 0;
    }
    std::size_t written = 0;
    std::size_t pos = 0;
    bool ok = true;
    ZSTD_decompressBegin(dctx);
    while (pos + 3 <= srcLen) {
        const std::uint32_t hdr = static_cast<std::uint32_t>(src[pos]) |
                                  (static_cast<std::uint32_t>(src[pos + 1]) << 8) |
                                  (static_cast<std::uint32_t>(src[pos + 2]) << 16);
        pos += 3;
        const bool last = (hdr & 1u) != 0;
        const std::uint32_t type = (hdr >> 1) & 3u;
        std::uint32_t size = hdr >> 3;
        if (type == 2 && size == 0) {
            size = static_cast<std::uint32_t>(src[pos]) |
                   (static_cast<std::uint32_t>(src[pos + 1]) << 8) |
                   (static_cast<std::uint32_t>(src[pos + 2]) << 16) |
                   (static_cast<std::uint32_t>(src[pos + 3]) << 24);
            pos += 4;
        }
        if (size > srcLen - pos) {
            err = "zstd 裸块长度越界";
            ok = false;
            break;
        }
        const std::size_t room = dstCapacity - written;
        std::size_t produced = 0;
        if (type == 2) {
            const int r = ZSTD_decompressBlock(dctx, dst + written, room, src + pos, size);
            if (ZSTD_isError(r)) {
                err = std::string("zstd 裸块解压失败: ") + ZSTD_getErrorName(r);
                ok = false;
                break;
            }
            produced = static_cast<std::size_t>(r);
        } else if (type == 0) {
            produced = size < room ? size : room;
            std::memcpy(dst + written, src + pos, produced);
        } else {
            const std::uint8_t byte = size > 0 ? src[pos] : 0;
            produced = size < room ? size : room;
            std::memset(dst + written, byte, produced);
        }
        written += produced;
        pos += size;
        if (last) {
            break;
        }
    }
    ZSTD_freeDCtx(dctx);
    return ok ? written : 0;
}

}  // namespace

std::size_t decompressByMethod(std::uint8_t id, const std::uint8_t* src, std::size_t srcLen,
                               std::uint8_t* dst, std::size_t dstCapacity, std::string& err) {
    switch (id) {
        case kMethodLz4:
        case kMethodLz4Hc: {
            const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(src),
                                              reinterpret_cast<char*>(dst),
                                              static_cast<int>(srcLen),
                                              static_cast<int>(dstCapacity));
            if (n <= 0) {
                err = std::string(methodName(id)) + ": LZ4_decompress_safe 返回 " + std::to_string(n);
                return 0;
            }
            return static_cast<std::size_t>(n);
        }
        case kMethodZstd: {
            std::size_t n = ZSTD_decompress(dst, dstCapacity, src, srcLen);
            if (!ZSTD_isError(n)) {
                return n;
            }
            // 同一个 method 声明下容器有两种形态：包 7 全部是无帧裸块流。
            std::string rawErr;
            const std::size_t raw = decodeZstdRawStream(src, srcLen, dst, dstCapacity, rawErr);
            if (raw != 0) {
                return raw;
            }
            err = std::string("zstd 帧: ") + ZSTD_getErrorName(n) + " / 裸块流: " + rawErr;
            return 0;
        }
        case kMethodOodle: {
            const int n = ooz::Decompress(src, srcLen, dst, dstCapacity);
            if (n <= 0) {
                err = "oodle: ooz::Decompress 返回 " + std::to_string(n);
                return 0;
            }
            return static_cast<std::size_t>(n);
        }
        case kMethodLz3:
        default:
            err = std::string("method ") + std::to_string(id) + " (" + methodName(id) +
                  ") 尚未接入";
            return 0;
    }
}

}  // namespace qtsvfs
