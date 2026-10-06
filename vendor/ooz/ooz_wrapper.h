#pragma once
/* ooz_wrapper.h — ooz 真实 OodleLZ 压缩/解压的轻量桥接 API
 *
 * 封装 rarten/ooz（powzix/ooz 增强 fork）：
 *   - 压缩：Kraken(8) / Mermaid(9) / Selkie(11) / Hydra(12) / Leviathan(13)
 *   - 解压：自动识别块头格式（Kraken/Mermaid/Leviathan/LZNA/Bitknit）
 * 输出为官方 Oodle 容器格式（与 libOodle 字节流兼容）。
 *
 * Hydra(12) 为真实 Hydra 编码器：整合 ooz Kraken/Mermaid/Leviathan
 * 逐 256KB 块取最优写入，块头保留各自 decoder_type（6/10/12）。
 */

#include <cstdint>
#include <cstddef>

namespace ooz {

// codec_id（对应 OodleLZ_Compressor 官方编号）
enum Codec {
    kCodecKraken    = 8,
    kCodecMermaid   = 9,
    kCodecSelkie    = 11,
    kCodecHydra     = 12,
    kCodecLeviathan = 13,
};

// 压缩。返回压缩后字节数；失败返回负值。
// 输出为官方 Oodle 块流（CompressBlock 格式，不带大小前缀头）。
// dst 所需容量可用 CompressedSizeNeeded() 查询。
int Compress(int codec_id, const uint8_t* src, size_t src_size,
             uint8_t* dst, size_t dst_capacity, int level);

// 解压。自动识别块头 decoder_type（6=Kraken,10=Mermaid,12=Leviathan,
// 5=LZNA,11=Bitknit）。成功返回解压字节数，失败返回负值。
// 输入为官方 Oodle 块流（不带大小前缀头）。
int Decompress(const uint8_t* src, size_t src_size,
               uint8_t* dst, size_t dst_size);

// 压缩所需的最大输出缓冲大小（官方公式：size + 274 * 块数）
size_t CompressedSizeNeeded(size_t src_size);

// 版本标识
const char* Version();

} // namespace ooz
