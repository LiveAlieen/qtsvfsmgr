#include "qtsvfs/codec/Codec.h"

#include <cstring>

extern "C" {
#include "lz4.h"
}
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
            const std::size_t n = ZSTD_decompress(dst, dstCapacity, src, srcLen);
            if (ZSTD_isError(n)) {
                err = std::string("zstd: ") + ZSTD_getErrorName(n);
                return 0;
            }
            return n;
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
