#include "qtsvfs/codec/Codec.h"

#include <cstring>

extern "C" {
#include "lz4.h"
#include "zstd.h"
}
#include "ooz_wrapper.h"

namespace qtsvfs {

const char* methodName(std::uint8_t id) {
    switch (id) {
        case kMethodRaw: return "raw";
        case kMethodLz4: return "lz4";
        case kMethodLz4Hc: return "lz4hc";
        case 3: return "oodle#3";
        case 4: return "oodle#4";
        case 5: return "oodle#5";
        case 6: return "oodle#6";
        case kMethodLz3: return "lz3";
        case kMethodLz3Alt: return "lz3?";
        case kMethodZstd: return "zstd";
        default: return "未知 method";
    }
}

std::size_t decompressByMethod(std::uint8_t id, const std::uint8_t* src, std::size_t srcLen,
                               std::uint8_t* dst, std::size_t dstCapacity, std::string& err) {
    if (id == kMethodRaw) {
        const std::size_t n = srcLen < dstCapacity ? srcLen : dstCapacity;
        std::memcpy(dst, src, n);
        return n;
    }
    if (id == kMethodLz4 || id == kMethodLz4Hc) {
        // ctx=0 分支：LZ4_decompress_fast，长度由页头声明的未压缩大小给出，流内无长度前缀。
        // 写侧 LZ4 与 LZ4-HC 产出的块由同一个解压处理函数承接，这里也走同一条路。
        // 注意：LZ4_decompress_fast 的返回值是**读掉的源字节数**，不是输出长度；
        // 输出长度就是传入的 originalSize（页里声明的未压缩大小）。
        const int n = LZ4_decompress_fast(reinterpret_cast<const char*>(src),
                                         reinterpret_cast<char*>(dst),
                                         static_cast<int>(dstCapacity));
        if (n < 0) {
            err = std::string(methodName(id)) + ": LZ4_decompress_fast 失败";
            return 0;
        }
        return dstCapacity;
    }
    if (id >= kMethodOodleMin && id <= kMethodOodleMax) {
        // OodleLZ_Decompress：ooz 自识别块头里的 decoder_type，不必先知道是哪个 Oodle 变体。
        const int n = ooz::Decompress(src, srcLen, dst, dstCapacity);
        if (n <= 0) {
            err = std::string(methodName(id)) + ": ooz::Decompress 返回 " + std::to_string(n);
            return 0;
        }
        return static_cast<std::size_t>(n);
    }
    if (id == kMethodZstd) {
        // 9 号页的流以 28 B5 2F FD（ZSTD 帧魔数）开头，是完整帧而非裸块。
        const size_t n = ZSTD_decompress(dst, dstCapacity, src, srcLen);
        if (ZSTD_isError(n)) {
            err = std::string("zstd: ") + ZSTD_getErrorName(n);
            return 0;
        }
        return static_cast<std::size_t>(n);
    }
    // 7/8 是 DecompressFunc_LZ3 的两个分支，但本 dump 里还没出现过 7/8 的块，
    // 且转储只给出 `*piVar == 7` 的二分、没给出 7/8 与 LZ3/LZ3HUF 的对应，故不接入。
    err = "method " + std::to_string(id) + " (" + methodName(id) + ") 尚未接入或号外";
    return 0;
}

}  // namespace qtsvfs
