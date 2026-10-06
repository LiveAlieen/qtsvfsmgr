#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qtsvfs {

// Qtsf 序列化流：4 字节对齐字段，8 字节字段对齐到 8；字符串为 [u32 长度][字节，无 NUL][补齐]。
// 依据 libQtsVFS.so.c 的 FUN_0030e25c(写) / FUN_0030e70c(读) 逐字段核对。
class QtsfStream {
public:
    QtsfStream(const std::uint8_t* data, std::size_t len) : base_(data), p_(data), end_(data + len) {}

    std::uint32_t u32();
    std::uint64_t u64();
    std::string str();
    std::vector<std::uint8_t> raw(std::size_t n);

    bool ok() const noexcept { return ok_; }
    std::size_t pos() const noexcept { return static_cast<std::size_t>(p_ - base_); }
    std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - p_); }

private:
    void alignUp(std::size_t a);

    const std::uint8_t* base_;
    const std::uint8_t* p_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

}  // namespace qtsvfs
