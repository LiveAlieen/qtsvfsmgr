#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qtsvfs {

// 页解压分派（libQtsVFS.so.c 实测语义，非猜测）：
//   DecompressPage 用 (&PTR_FUN_004bd880)[param_6 & 0xff] 取函数，param_6 是 FileNode
//   块描述符里的 packed；ctx = (packed>>8)&0xff 决定是否跨页续流。
//   QtsfTool::DecompressBuffer 明示：method 2/3/4/5 → OodleLZ_Decompress，
//   method 1 → LZ4_decompress_fast，index 0 → 裸拷贝。
enum : std::uint8_t {
    kMethodRaw = 0,
    kMethodLz4 = 1,
    kMethodOodleMin = 2,
    kMethodOodleMax = 5,
};

const char* methodName(std::uint8_t id);

// src/srcLen 是去掉页头 u32 之后的压缩流，dstCapacity 用页头声明的未压缩长度。
std::size_t decompressByMethod(std::uint8_t id, const std::uint8_t* src, std::size_t srcLen,
                               std::uint8_t* dst, std::size_t dstCapacity, std::string& err);

}  // namespace qtsvfs
