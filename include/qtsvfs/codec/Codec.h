#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qtsvfs {

// 页解压分派表（libQtsVFS.so.c，DecompressPage = FUN_00245ce8）：
//   (&PTR_FUN_004bd880)[packed & 0xff] 取页处理函数，ctx = (packed>>8)&0xff。
// 依据（全部来自声明，不是嗅探）：
//   0 = 裸拷贝；1 = LZ4；2 = LZ4-HC —— 与 1 同一个解压处理函数 FUN_00252ed4，
//       其错误串 "Not implement compress ctx type (%d) for lz4(hc)!" 表明两者共用。
//   3..6 = Oodle —— 写侧 CompressFunc_OODLE(FUN_00245040, L123333) 的守卫
//       `if ((method - 3) < 4)` 直接给出区间。
//   7/8 = LZ3 家族 —— DecompressFunc_LZ3(FUN_002535a8) 内 `if (*piVar == 7)` 分两个变体，
//       另一个即 8；具体谁对应 LZ3 还是 LZ3HUF 尚未在数据里出现，故只标 "lz3*"。
//   9 = ZSTD —— 实测包 0 节点 0342975E2CAD270C 的 9 号页流以 28 B5 2F FD（ZSTD 帧魔数）开头。
enum : std::uint8_t {
    kMethodRaw = 0,
    kMethodLz4 = 1,
    kMethodLz4Hc = 2,
    kMethodOodleMin = 3,
    kMethodOodleMax = 6,
    kMethodLz3 = 7,
    kMethodLz3Alt = 8,
    kMethodZstd = 9,
};

const char* methodName(std::uint8_t id);

// src/srcLen 是去掉页头 u32 之后的压缩流，dstCapacity 用页头声明的未压缩长度。
std::size_t decompressByMethod(std::uint8_t id, const std::uint8_t* src, std::size_t srcLen,
                               std::uint8_t* dst, std::size_t dstCapacity, std::string& err);

}  // namespace qtsvfs
