#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qtsvfs {

// 页解压分派：libQtsVFS.so 里 DecompressPage 用 (&PTR_FUN_004bd880)[flag & 0xff] 取函数，
// 表体在 .bss 由运行期填充，转储里没有写点；编号顺序按压缩侧表 PTR_FUN_004bd668
// （DecompressFunc_LZ4 / LZ4HC / OODLE / LZ3 / ZSTD）对齐，并用包 0 的两个
// node-index 样本实测校验（见 codec/Codec.cpp 的注释）。
enum : std::uint8_t {
    kMethodLz4 = 0,
    kMethodLz4Hc = 1,
    kMethodOodle = 2,
    kMethodLz3 = 3,
    kMethodZstd = 4,
};

const char* methodName(std::uint8_t id);

// 解压到 dst（容量 dstCapacity）；返回实际字节数，失败返回 0 并写 err。
std::size_t decompressByMethod(std::uint8_t id, const std::uint8_t* src, std::size_t srcLen,
                               std::uint8_t* dst, std::size_t dstCapacity, std::string& err);

}  // namespace qtsvfs
