#include "qtsvfs/format/PathHash.h"

namespace qtsvfs {

std::uint64_t calcHashCode64Raw(const std::uint8_t* data, std::size_t len) {
    if (len == 0) {
        return (static_cast<std::uint64_t>(kHashSeedHi) << 32) | kHashSeedLo;
    }
    std::uint32_t hi = kHashSeedHi;
    std::uint32_t lo = kHashSeedLo;
    std::size_t f = 0;
    std::size_t b = len;
    std::size_t next;
    do {
        --b;
        next = f + 1;
        lo = lo * 0x21u ^ data[f];
        hi = hi * 0x21u ^ data[b];
        f = next;
    } while (next < len);
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

}  // namespace qtsvfs
