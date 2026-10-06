#include "ooz_wrapper.h"
#include "stdafx.h"
#include "compress.h"

#include <cstring>
#include <climits>
#include <vector>

// GetCompressedBufferSizeNeeded 定义于 compress.cpp（未在头文件声明）
extern int GetCompressedBufferSizeNeeded(int size);

// Kraken_Decompress 定义于 kraken.cpp（C++ 链接，无独立头文件）
int Kraken_Decompress(const uint8_t* src, size_t src_len,
                      uint8_t* dst, size_t dst_len);

// Kraken_ParseHeader 定义于 kraken.cpp：解析 Oodle 块头，返回 decoder_type。
// 在 ooz_wrapper 层复用其检测 LZNA(5)/Bitknit(11) 是否需要 SAFE_SPACE。
struct KrakenHeader {
    int decoder_type;
    bool restart_decoder;
    bool uncompressed;
    bool use_checksums;
};
const byte* Kraken_ParseHeader(KrakenHeader* hdr, const byte* p);

namespace ooz {

// 对齐反编译 DecompressFunc_OODLE 的 fuzzSafe=1 语义：LZNA(5)/Bitknit(11)
// 解码器在跨块回看匹配时会读 dst 起点之前的少量字节（lzna.cpp/bitknit.cpp
// 中的 `dst - dist`），需在 dst 起点前预留安全空间，避免越界读。
// 该值取 64 与 test_ooz.cpp / 官方 CLI 安全空间约定一致（见 lzna.cpp:379 等）。
static const size_t kOodleSafeSpace = 64;

// 判断输入块流是否需要 SAFE_SPACE（仅 LZNA=5 / Bitknit=11 需要）。
//
// 注意（Issue #61）：SAFE_SPACE 只挡住「起点之前的回看」，挡不住**块内任意距离**
// 的越界读写。当块头 decoder_type 与真实解码器不符（伪造/损坏块）、或距离模型被
// 脏数据解出非法值时，`dst - dist` 会指向缓冲之外，实测直接段错误。因此：
//   · 调用方（页解压）必须按块头**精确**算法创建解码器（不得一律用 Kraken）；
//   · LZNA/Bitknit 的 RANS 首字节读取与 nibble 查表已加输入长度/掩码保护
//     （见 lzna.cpp 的 LznaBitReader_Init 与 LznaReadNibble，qtsvfs 修复），
//     使其对坏数据返回失败而不是踩进未定义行为。
static bool NeedsSafeSpace(const uint8_t* src, size_t src_size) {
    if (src_size < 2) return false;
    KrakenHeader hdr;
    const byte* p = Kraken_ParseHeader(&hdr, src);
    if (!p) return false;
    return (hdr.decoder_type == 5 || hdr.decoder_type == 11);
}

int Compress(int codec_id, const uint8_t* src, size_t src_size,
             uint8_t* dst, size_t dst_capacity, int level) {
    if (!src || !dst || src_size == 0 || dst_capacity == 0) return -1;
    if (src_size > (size_t)INT32_MAX) return -1;

    // 参考 ooz CLI：输入 buffer 预留 65536 额外空间用于窗口回看
    // （真实编码器在读取 src 时可能回溯 window_base 之前的字节）。
    // 这里按 CompressBlock 的语义：src_window_base == src_in 时安全。
    int n = CompressBlock(codec_id,
                          const_cast<uint8_t*>(src),
                          dst,
                          (int)src_size,
                          level,
                          nullptr,   // compressopts -> 默认
                          nullptr,   // src_window_base -> src_in
                          nullptr);  // lrm
    if (n < 0 || (size_t)n > dst_capacity) return -1;
    return n;
}

int Decompress(const uint8_t* src, size_t src_size,
               uint8_t* dst, size_t dst_size) {
    if (!src || !dst || src_size == 0) return -1;
    if (src_size > (size_t)INT32_MAX || dst_size > (size_t)INT32_MAX) return -1;

    // 对齐反编译 DecompressFunc_OODLE 的 fuzzSafe=1 语义：
    // 对 LZNA/Bitknit 老格式（decoder_type 5/11），解码器在跨块回看时会读
    // dst 起点之前的字节（`dst - dist`）。此处为调用方目标缓冲自动预留
    // 64 字节 SAFE_SPACE：内部分配 dst_size+kOodleSafeSpace 缓冲、把解码起点
    // 前移 kOodleSafeSpace 字节，使回看落在缓冲内，再拷贝回调用方 dst。
    // 该路径仅在 LZNA/Bitknit 触发，Kraken/Mermaid/Leviathan 不回读、零拷贝直解。
    if (NeedsSafeSpace(src, src_size)) {
        std::vector<uint8_t> safe(dst_size + kOodleSafeSpace);
        uint8_t* dst_start = safe.data() + kOodleSafeSpace;
        int n = Kraken_Decompress(src, src_size, dst_start, dst_size);
        if (n > 0)
            std::memcpy(dst, dst_start, static_cast<size_t>(n));
        return n;
    }

    // 直接调用官方块流解压器（不依赖外部大小前缀头）
    return Kraken_Decompress(src, src_size, dst, dst_size);
}

size_t CompressedSizeNeeded(size_t src_size) {
    return GetCompressedBufferSizeNeeded((int)src_size);
}

const char* Version() {
    return "ooz v7.1 (rarten/ooz - native OodleLZ + qtsvfs Hydra)";
}

} // namespace ooz
