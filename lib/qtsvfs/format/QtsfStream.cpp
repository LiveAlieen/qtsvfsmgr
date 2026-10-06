#include "qtsvfs/format/QtsfStream.h"

#include <cstring>

namespace qtsvfs {
namespace {
std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
}  // namespace

void QtsfStream::alignUp(std::size_t a) {
    const std::size_t rem = static_cast<std::size_t>(p_ - base_) % a;
    if (rem != 0) {
        const std::size_t pad = a - rem;
        if (pad > static_cast<std::size_t>(end_ - p_)) {
            ok_ = false;
            p_ = end_;
            return;
        }
        p_ += pad;
    }
}

std::uint32_t QtsfStream::u32() {
    alignUp(4);
    if (static_cast<std::size_t>(end_ - p_) < 4) {
        ok_ = false;
        return 0;
    }
    const std::uint32_t v = readLe32(p_);
    p_ += 4;
    return v;
}

std::uint64_t QtsfStream::u64() {
    alignUp(8);
    if (static_cast<std::size_t>(end_ - p_) < 8) {
        ok_ = false;
        return 0;
    }
    const std::uint64_t v = static_cast<std::uint64_t>(readLe32(p_)) |
                            (static_cast<std::uint64_t>(readLe32(p_ + 4)) << 32);
    p_ += 8;
    return v;
}

std::vector<std::uint8_t> QtsfStream::raw(std::size_t n) {
    if (n > static_cast<std::size_t>(end_ - p_)) {
        ok_ = false;
        return {};
    }
    std::vector<std::uint8_t> out(p_, p_ + n);
    p_ += n;
    return out;
}

std::string QtsfStream::str() {
    const std::uint32_t len = u32();
    if (!ok_) {
        return {};
    }
    auto bytes = raw(len);
    if (!ok_) {
        return {};
    }
    while (!bytes.empty() && bytes.back() == 0) {
        bytes.pop_back();
    }
    alignUp(4);
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace qtsvfs
