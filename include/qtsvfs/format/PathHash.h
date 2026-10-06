#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace qtsvfs {

// 移植自 libQtsVFS.so 导出函数 VFS_CalcHashCode64Raw。
// 前向累加进低位、反向累加进高位，长度为奇数时中间字节两边各吃一次。
// 空串返回 kEmpty。
inline constexpr std::uint32_t kHashSeedLo = 0x5bd1e995u;
inline constexpr std::uint32_t kHashSeedHi = 0xab9423a7u;

std::uint64_t calcHashCode64Raw(const std::uint8_t* data, std::size_t len);

inline std::uint64_t calcHashCode64(std::string_view path) {
    return calcHashCode64Raw(reinterpret_cast<const std::uint8_t*>(path.data()), path.size());
}

}  // namespace qtsvfs
