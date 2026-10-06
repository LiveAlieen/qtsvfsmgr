// stdafx.h : includefile for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#define _CRT_SECURE_NO_WARNINGS 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <limits.h>

// MSVC / MinGW-w64: 使用 <intrin.h> 提供的 _rotl/_byteswap_*/_BitScan* 等内建函数。
// 注意 MinGW-w64 的 <stdlib.h> 会先声明 extern 的 _rotl，因此不能像 GCC 分支那样
// 再定义 static inline 的同名函数，否则报 "'_rotl' was declared 'extern' and later 'static'"。
// 而 <intrin.h> 在 MinGW-w64 下兼容，可同时覆盖 MSVC 与 MinGW-w64。
#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
#include <intrin.h>
#undef max
#undef min
#else
#include <stddef.h>
#define __forceinline inline
#define _byteswap_ushort(x) __builtin_bswap16((uint16)(x))
#define _byteswap_ulong(x) __builtin_bswap32((uint32)(x))
#define _byteswap_uint64(x) __builtin_bswap64((uint64)(x))
#define _BitScanForward(dst, x) (*(dst) = __builtin_ctz(x))
#define _BitScanReverse(dst, x) (*(dst) = (__builtin_clz(x) ^ 31))

static inline uint32_t _rotl(uint32_t x, int n) {
  return (((x) << (n)) | ((x) >> (32-(n))));
}

#include <xmmintrin.h>
#endif

// 【Windows/MinGW 可移植性】#pragma warning 是 MSVC 专属语法，MinGW/GCC 会报
// `ignoring '#pragma warning' [-Wunknown-pragmas]`（每个 TU 重复 3 条，噪音明显）。
// 用 _MSC_VER 门控：GCC 分支改用等价的 -Wno-xxx 抑制（见 ooz/CMakeLists.txt）。
#ifdef _MSC_VER
#pragma warning (disable: 4244)
#pragma warning (disable: 4530) // c++ exception handler used without unwind semantics
#pragma warning (disable: 4018) // signed/unsigned mismatch
#endif

// TODO: reference additional headers your program requires here
typedef uint8_t byte;
typedef uint8_t uint8;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef int32_t int32;
typedef uint16_t uint16;
typedef int16_t int16;
typedef unsigned int uint;
