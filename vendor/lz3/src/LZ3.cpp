#include "qtsvfs_lz3/LZ3.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

namespace qtsvfs_lz3 {

// =====================================================================
// 内部工具
// =====================================================================
namespace {

// 输出缓冲（带容量检查）
class ByteWriter {
public:
    ByteWriter(uint8_t* dst, size_t cap) : dst_(dst), cap_(cap), pos_(0) {}
    size_t size() const { return pos_; }
    bool put8(uint8_t v) {
        if (pos_ >= cap_) return false;
        dst_[pos_++] = v;
        return true;
    }
    bool put16le(uint16_t v) {
        if (pos_ + 2 > cap_) return false;
        dst_[pos_++] = static_cast<uint8_t>(v & 0xff);
        dst_[pos_++] = static_cast<uint8_t>(v >> 8);
        return true;
    }
    bool putBytes(const uint8_t* src, size_t n) {
        if (pos_ + n > cap_) return false;
        std::memcpy(dst_ + pos_, src, n);
        pos_ += n;
        return true;
    }

private:
    uint8_t* dst_;
    size_t cap_;
    size_t pos_;
};

// 符号表解码（对齐反编译 FUN_003a6634 开头段）
// 返回消耗的字节数（不含 symCount 字节）；失败返回 0。
size_t decodeSymbols(const uint8_t* src, size_t srcSize, size_t symCount,
                     std::vector<uint16_t>& syms) {
    size_t p = 0;
    syms.clear();
    syms.reserve(symCount);
    for (size_t i = 0; i < symCount; ++i) {
        if (p >= srcSize) return 0;
        uint8_t b = src[p++];
        if (b < 0x80) {
            syms.push_back(b);
        } else {
            if (p >= srcSize) return 0;
            uint8_t b2 = src[p++];
            syms.push_back(static_cast<uint16_t>((b & 0x7f) | ((b2 ^ 1) << 7)));
        }
    }
    return p;
}

// =====================================================================
// 哈希链（16bit 哈希，用于快速收集匹配候选）
// =====================================================================
struct HashChain {
    const uint8_t* data;
    size_t size;
    std::vector<size_t> head;  // 哈希桶 -> 最新位置
    std::vector<size_t> next;  // 链：位置 -> 上一个同哈希位置

    static constexpr size_t kHashBits = 16;
    static constexpr size_t kHashSize = 1u << kHashBits;
    static constexpr size_t kNone = std::numeric_limits<size_t>::max();

    HashChain(const uint8_t* d, size_t n) : data(d), size(n) {
        head.assign(kHashSize, kNone);
        next.assign(n, kNone);
    }

    // 上界守卫：尾部不足 3 字节时用 0 补齐，避免越界读（ASan heap-buffer-overflow）。
    // 对 pos + 2 < size 的位置哈希值与旧实现完全一致，故不影响既有压缩产物。
    uint32_t hash3(size_t pos) const {
        const uint32_t b0 = pos < size ? data[pos] : 0u;
        const uint32_t b1 = pos + 1 < size ? data[pos + 1] : 0u;
        const uint32_t b2 = pos + 2 < size ? data[pos + 2] : 0u;
        return ((b0 * 0x1E35A7BDu) ^ (b1 * 0x1E35A7BDu >> 7) ^ (b2 << 5)) &
               (kHashSize - 1);
    }

    void insert(size_t pos) {
        uint32_t h = hash3(pos);
        next[pos] = head[h];
        head[h] = pos;
    }
};

// =====================================================================
// 解析结果：一系列操作（字面量运行 + 可选匹配）
// =====================================================================
struct Op {
    size_t litPos;    // 字面量在源中的偏移
    size_t litLen;    // 字面量长度
    uint32_t dist;    // 匹配距离（无匹配为 0）
    size_t matchLen;  // 匹配长度（无匹配为 0）
};

// ---- 代价模型（单位 1/0x100 字节；对齐反编译代价表公式） ----

// 匹配长度扩展字节数（>=18 时 nibble=0xf，起始 0，每 255 追加 1 字节）
static int32_t matchExtBytes(size_t mlen) {
    if (mlen < 18) return 0;
    return static_cast<int32_t>(1 + (mlen - 18) / 0xff);
}

// 偏移编码成本：符号表索引固定 2 字节（token 内），内联 2 字节 + 1 修正字节
static constexpr int32_t kSymOffsetCost = 0x200;    // 2B
static constexpr int32_t kInlineOffsetCost = 0x300; // 3B
static constexpr int32_t kTokenCost = 0x200;        // token 2B
static constexpr int32_t kLitFirstByteCost = 0x100; // 字面量首字节 1B

// =====================================================================
// 贪心解析（仅用于收集距离频率 / 预符号表；正式输出不使用）
// =====================================================================
std::vector<Op> parseGreedy(const uint8_t* src, size_t n) {
    std::vector<Op> ops;
    if (n == 0) return ops;
    HashChain parser(src, n);
    size_t pos = 0;
    size_t litStart = 0;
    while (pos < n) {
        const size_t maxLen = std::min(n - pos, static_cast<size_t>(kLZ3MaxMatch));
        uint32_t bestDist = 0;
        size_t bestLen = 0;
        const size_t winMin = pos > kLZ3MaxDist ? pos - kLZ3MaxDist : 0;
        const uint32_t h = parser.hash3(pos);
        size_t cand = parser.head[h];
        size_t checked = 0;
        while (cand != HashChain::kNone && checked < 4096) {
            if (cand < winMin) break;
            const uint32_t dist = static_cast<uint32_t>(pos - cand);
            if (dist != 0) {
                size_t len = 0;
                while (len < maxLen && src[cand + len] == src[pos + len]) ++len;
                if (len > bestLen) {
                    bestLen = len;
                    bestDist = dist;
                    if (len == maxLen) break;
                }
            }
            cand = parser.next[cand];
            ++checked;
        }
        if (bestLen >= kLZ3MinMatch) {
            ops.push_back({litStart, pos - litStart, bestDist, bestLen});
            for (size_t i = pos; i < pos + bestLen && i + kLZ3MinMatch <= n; ++i) {
                parser.insert(i);
            }
            pos += bestLen;
            litStart = pos;
        } else {
            if (pos + kLZ3MinMatch <= n) parser.insert(pos);
            ++pos;
        }
    }
    if (litStart < n) {
        ops.push_back({litStart, n - litStart, 0, 0});
    }
    return ops;
}

// =====================================================================
// 频率符号表构建
//
// 对齐反编译 FUN_003a42c4 / FUN_003a4704 的频率/前缀思想：
//   - 统计每个偏移距离在 op 序列中的出现次数；
//   - 按频率降序取前 kLZ3MaxSym 个偏移，保证 token 内 7bit 索引可用；
//   - 高频偏移进符号表 → 2 字节编码；低频/未收录 → 内联 3 字节。
// =====================================================================
std::vector<uint16_t> buildSymbolTable(const std::vector<Op>& ops) {
    std::vector<uint16_t> syms;
    std::vector<uint32_t> freq(kLZ3MaxDist + 1, 0);
    for (const Op& op : ops) {
        if (op.matchLen > 0 && op.dist <= kLZ3MaxDist) {
            ++freq[op.dist];
        }
    }
    std::vector<uint32_t> idx;
    idx.reserve(kLZ3MaxDist + 1);
    for (uint32_t d = 1; d <= kLZ3MaxDist; ++d) {
        if (freq[d] > 0) idx.push_back(d);
    }
    std::stable_sort(idx.begin(), idx.end(), [&freq](uint32_t a, uint32_t b) {
        return freq[a] > freq[b];
    });
    const size_t take = std::min(idx.size(), static_cast<size_t>(kLZ3MaxSym));
    syms.reserve(take);
    for (size_t i = 0; i < take; ++i) syms.push_back(static_cast<uint16_t>(idx[i]));
    return syms;
}

// =====================================================================
// DP 最优解析
//
// LZ3 字节格式约束：每个 op = [字面量运行][匹配] 共用一个 token；只有块尾
// 允许纯字面量 token（无匹配、无偏移）。因此中间状态只允许两种完整 op：
//   A) 纯字面量 op —— 仅限块尾；
//   B) 运行 + 匹配 op —— 中间 op 必须带匹配。
//
// 状态：
//   e[i] = 覆盖 src[0..i) 且 i 为 op 边界的最优代价（op 已完整写出）
//   r[i] = 覆盖 src[0..i) 且最后一个 op 是"开放字面量运行"的代价
//          （运行起点为 runStart[i]，尚未追加匹配）
// 转移（代价单位 1/0x100 字节，与 LZ3 字节格式精确对应）：
//   e[i] --起新运行-->     r[i+1]  + token(2B) + 首字节(1B) = +0x300
//   r[i] --续写-->         r[i+1]  + 数据(1B)，跨 15 / 15+255k 边界追加扩展字节
//   e[i] --空运行+匹配-->  e[i+M]  + token(2B) + 偏移 + 匹配扩展
//   r[i] --运行+匹配-->    e[i+M]  + 偏移 + 匹配扩展（token 已在运行起点计入）
//   收尾：r[n] --闭合-->   e[n]    + 0（块尾纯字面量 op）
//
// 注意：中间位置不做 r[i] -> e[i] 闭合（那会产生无匹配的中间 op）。
// 符号表感知：偏移若在预符号表 syms 中按 2B 计，否则按内联 3B 计。
// =====================================================================
struct Back {
    int32_t runStart;  // op 起点（字面量运行起点）
    uint32_t dist;     // 匹配距离（无匹配为 0）
    uint32_t len;      // 匹配长度（0 表示块尾纯字面量 op）
};

std::vector<Op> parseDP(const uint8_t* src, size_t n, bool hc,
                        const std::vector<uint16_t>& syms) {
    std::vector<Op> ops;
    if (n == 0) return ops;

    const size_t kMaxCand = hc ? 256 : 32;  // HC 变体更宽的候选搜索
    const int32_t kInf = 0x7fffffff;

    std::vector<int32_t> e(n + 1, kInf);
    std::vector<int32_t> r(n + 1, kInf);
    std::vector<int32_t> runStart(n + 1, -1);
    std::vector<Back> decE(n + 1);

    HashChain chain(src, n);

    // 符号表成员查找表（避免线性查找）
    std::vector<char> isSym(kLZ3MaxDist + 1, 0);
    for (uint16_t s : syms) isSym[s] = 1;
    auto inSyms = [&isSym](uint32_t d) { return d <= kLZ3MaxDist && isSym[d] != 0; };

    e[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        // 0) 续写字面量运行（数据字节；跨 15 / 15+255k 边界追加扩展字节）
        if (i >= 1 && r[i - 1] < kInf) {
            const int32_t rs = runStart[i - 1];
            const int32_t newLen = static_cast<int32_t>(i + 1) - rs;
            int32_t c = r[i - 1] + kLitFirstByteCost;
            if (newLen == 15 || (newLen > 15 && (newLen - 15) % 255 == 0))
                c += kLitFirstByteCost;
            if (c < r[i]) {
                r[i] = c;
                runStart[i] = rs;
            }
        }

        // 注意：中间位置不做 r[i] -> e[i] 闭合（会产生无匹配的中间 op）
        if (e[i] >= kInf && r[i] >= kInf) continue;

        // 插入当前位到哈希链（供后续位置匹配引用）
        if (i + kLZ3MinMatch <= n) chain.insert(i);

        const int32_t ei = e[i];

        // 1) 起新字面量运行：token + 首字节（仅从 e[i]，因为中间 op 必须带匹配，
        //    开放运行必须最终接匹配；r[i] 已存在时不覆盖）
        if (e[i] < kInf && ei + kTokenCost + kLitFirstByteCost < r[i + 1]) {
            r[i + 1] = ei + kTokenCost + kLitFirstByteCost;
            runStart[i + 1] = static_cast<int32_t>(i);
        }

        // 2) 匹配转移（中间 op 必须带匹配）
        const size_t rem = n - i;
        if (rem >= kLZ3MinMatch) {
            const size_t maxLen = std::min(rem, static_cast<size_t>(kLZ3MaxMatch));
            const size_t winMin = i > kLZ3MaxDist ? i - kLZ3MaxDist : 0;
            const uint32_t h = chain.hash3(i);
            size_t cand = chain.head[h];
            size_t checked = 0;
            // 逐候选评估（保留灵活性）；匹配长度 >= 64 时提前终止（足够优）
            while (cand != HashChain::kNone && checked < kMaxCand) {
                if (cand < winMin) break;
                const uint32_t dist = static_cast<uint32_t>(i - cand);
                if (dist != 0 && dist <= kLZ3MaxDist) {
                    size_t len = 0;
                    // 快速比较：memcmp 加速（libc 向量化）
                    while (len + 8 <= maxLen &&
                           std::memcmp(src + cand + len, src + i + len, 8) == 0) len += 8;
                    while (len < maxLen && src[cand + len] == src[i + len]) ++len;
                    if (len >= kLZ3MinMatch) {
                        const int32_t offCost =
                            inSyms(dist) ? kSymOffsetCost : kInlineOffsetCost;
                        const int32_t ext = matchExtBytes(len) * kLitFirstByteCost;
                        // 从 e[i]：op = [空运行][匹配]（仅当 e[i] 可达）
                        if (e[i] < kInf) {
                            const int32_t c = ei + kTokenCost + offCost + ext;
                            if (c < e[i + len]) {
                                e[i + len] = c;
                                decE[i + len] = {static_cast<int32_t>(i), dist,
                                                 static_cast<uint32_t>(len)};
                            }
                        }
                        // 从 r[i]：op = [运行][匹配]（仅当 r[i] 可达）
                        if (r[i] < kInf) {
                            const int32_t c = r[i] + offCost + ext;
                            if (c < e[i + len]) {
                                e[i + len] = c;
                                decE[i + len] = {runStart[i], dist,
                                                 static_cast<uint32_t>(len)};
                            }
                        }
                        // 高质量匹配提前终止候选扫描（HC 阈值更高 → 搜索更彻底）
                        if (len >= (hc ? 128u : 64u)) break;
                    }
                }
                cand = chain.next[cand];
                ++checked;
            }
        }
    }

    // 收尾：r[n] 免费闭合为块尾纯字面量 op
    if (r[n] < e[n]) {
        e[n] = r[n];
        decE[n] = {runStart[n], 0, 0};
    }
    if (e[n] >= kInf) {
        // 无匹配时只能整块一个纯字面量 op
        ops.push_back({0, n, 0, 0});
        return ops;
    }

    // 回推最优 op 序列
    int32_t pos = static_cast<int32_t>(n);
    while (pos > 0) {
        const Back& b = decE[pos];
        if (b.len > 0) {
            // op = [运行][匹配]，匹配段在 [pos-len, pos)
            const size_t runEnd = static_cast<size_t>(pos) - b.len;
            ops.push_back({static_cast<size_t>(b.runStart),
                           runEnd - static_cast<size_t>(b.runStart), b.dist, b.len});
        } else {
            // 块尾纯字面量 op
            const size_t litLen = static_cast<size_t>(pos) - static_cast<size_t>(b.runStart);
            if (litLen > 0) {
                ops.push_back({static_cast<size_t>(b.runStart), litLen, 0, 0});
            }
        }
        pos = b.runStart;
    }
    std::reverse(ops.begin(), ops.end());
    return ops;
}

}  // namespace

// =====================================================================
// 单块压缩
// =====================================================================

size_t compressBlock(const uint8_t* src, size_t srcSize,
                     uint8_t* dst, size_t dstCapacity, int level) {
    if (src == nullptr || dst == nullptr) return 0;
    if (srcSize == 0 || srcSize > kLZ3MaxBlockSize) return 0;

    // level>=1 → LZ3HC 变体（更宽候选搜索）；level<=0 → LZ3（DP 标准搜索）
    const bool hc = level >= 1;

    // Pass A：贪心解析收集距离频率 → 预符号表（供 DP 偏移代价估计）
    std::vector<Op> greedy = parseGreedy(src, srcSize);
    std::vector<uint16_t> symsPre = buildSymbolTable(greedy);

    // Pass B：DP 最优解析（符号表感知偏移代价，精确长度扩展）
    std::vector<Op> ops = parseDP(src, srcSize, hc, symsPre);

    // 最终符号表：按 DP 结果的频率构建（编码以它为唯一依据）
    std::vector<uint16_t> syms = buildSymbolTable(ops);

    ByteWriter w(dst, dstCapacity);

    // 符号表
    if (!w.put8(static_cast<uint8_t>(syms.size()))) return 0;
    for (uint16_t v : syms) {
        if (v < 0x80) {
            if (!w.put8(static_cast<uint8_t>(v))) return 0;
        } else {
            if (!w.put8(static_cast<uint8_t>(0x80 | (v & 0x7f)))) return 0;
            if (!w.put8(static_cast<uint8_t>((v >> 7) ^ 1))) return 0;
        }
    }

    // 编码操作序列
    for (size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        const size_t litLen = op.litLen;
        const size_t mlen = op.matchLen;

        uint16_t token = 0;
        // 字面量长度 nibble（0..0xf）
        const uint16_t litNib = litLen < 0xf ? static_cast<uint16_t>(litLen) : 0xf;
        token |= litNib;

        // 匹配长度：3..16 → 0..13；17 → 0xe；>=18 → 0xf + 扩展
        if (mlen > 0) {
            if (mlen < 18) {
                token |= static_cast<uint16_t>((mlen - kLZ3MinMatch) << 4);
            } else {
                token |= 0xf0u;
            }
        }

        // 偏移：优先符号表（已收录），否则内联
        bool useSym = false;
        if (mlen > 0) {
            auto it = std::find(syms.begin(), syms.end(), static_cast<uint16_t>(op.dist));
            if (it != syms.end()) {
                useSym = true;
                const uint16_t idx = static_cast<uint16_t>(it - syms.begin());
                token |= 0x8000u;
                token |= static_cast<uint16_t>((idx & 0x7f) << 8);
            }
        }
        if (mlen > 0 && !useSym) {
            // 内联：bit8-14 = dist 高 7 位，bit15 = 0
            token |= static_cast<uint16_t>(((op.dist >> 8) & 0x7f) << 8);
        }

        if (!w.put16le(token)) return 0;

        // 字面量长度扩展（0xff 级联）
        if (litNib == 0xf) {
            size_t e = litLen - 0xf;
            while (e >= 0xff) {
                if (!w.put8(0xff)) return 0;
                e -= 0xff;
            }
            if (!w.put8(static_cast<uint8_t>(e))) return 0;
        }

        // 字面量字节
        if (!w.putBytes(src + op.litPos, litLen)) return 0;

        // 偏移修正字节（仅内联路径）
        if (mlen > 0 && !useSym) {
            const uint8_t extra = static_cast<uint8_t>((op.dist & 0xff) ^ (token & 0xff));
            if (!w.put8(extra)) return 0;
        }

        // 匹配长度扩展（>=18 时，0xff 级联，起始值 = mlen-18）
        if (mlen >= 18) {
            size_t e = mlen - 18;
            while (e >= 0xff) {
                if (!w.put8(0xff)) return 0;
                e -= 0xff;
            }
            if (!w.put8(static_cast<uint8_t>(e))) return 0;
        }
    }

    return w.size();
}

// =====================================================================
// 单块解压（对齐反编译 FUN_003a6634）
// =====================================================================

size_t decompressBlock(const uint8_t* src, size_t srcSize,
                       uint8_t* dst, size_t expectedSize,
                       size_t* consumed) {
    if (src == nullptr || dst == nullptr) return 0;
    if (srcSize == 0) return 0;

    size_t p = 0;

    // 符号表
    if (p >= srcSize) return 0;
    const uint8_t symCount = src[p++];
    if (symCount > kLZ3MaxSym) return 0;
    std::vector<uint16_t> syms;
    const size_t symBytes = decodeSymbols(src + p, srcSize - p, symCount, syms);
    // symCount==0 时合法（无符号表），symBytes 为 0；仅当声明了符号但解码失败才算错
    if ((symCount != 0 && symBytes == 0) || syms.size() != symCount) return 0;
    p += symBytes;

    size_t outPos = 0;

    while (outPos < expectedSize && p < srcSize) {
        // token
        if (p + 2 > srcSize) return 0;
        const uint16_t token =
            static_cast<uint16_t>(src[p]) | (static_cast<uint16_t>(src[p + 1]) << 8);
        p += 2;

        // 字面量长度
        uint32_t litLen = token & 0xf;
        if (litLen == 0xf) {
            litLen = 0xf;
            while (p < srcSize) {
                const uint8_t b = src[p++];
                litLen += b;
                if (b != 0xff) break;
            }
        }

        // 拷贝字面量
        if (litLen > expectedSize - outPos) return 0;
        if (p + litLen > srcSize) return 0;
        std::memcpy(dst + outPos, src + p, litLen);
        p += litLen;
        outPos += litLen;

        // 输出已满 → 块尾（纯字面量 token，无匹配）
        if (outPos == expectedSize) {
            if (consumed) *consumed = p;
            return outPos;
        }
        if (outPos > expectedSize) return 0;

            // 偏移（紧跟字面量之后）
        uint32_t dist;
        if (token & 0x8000u) {
            const uint32_t idx = (token >> 8) & 0x7f;
            if (idx >= syms.size()) return 0;
            dist = syms[idx];
        } else {
            if (p >= srcSize) return 0;
            const uint8_t extra = src[p++];
            dist = (((token >> 8) & 0x7f) << 8) | (extra ^ (token & 0xff));
        }
        if (dist == 0 || dist > outPos) return 0;

        // 匹配长度（对齐反编译：0xf 级联起始 15，最终 +3）
        const uint32_t mcode = (token >> 4) & 0xf;
        uint32_t mlen;
        if (mcode == 0xf) {
            mlen = 0xf;
            while (p < srcSize) {
                const uint8_t b = src[p++];
                mlen += b;
                if (b != 0xff) break;
            }
            mlen += 3;
        } else if (mcode == 0xe) {
            mlen = 17;
        } else {
            mlen = mcode + kLZ3MinMatch;
        }

        // 拷贝匹配（支持重叠）
        if (mlen > expectedSize - outPos) return 0;
        for (uint32_t i = 0; i < mlen; ++i) {
            dst[outPos] = dst[outPos - dist];
            ++outPos;
        }
    }

    if (outPos != expectedSize) return 0;
    if (consumed) *consumed = p;
    return outPos;
}

// =====================================================================
// 便捷接口
// =====================================================================

size_t maxCompressedSizeBlock(size_t srcSize) {
    // 最坏情况安全上界：每 1 字节字面量 1 token（2B）+ 字面量扩展（1B）+ 符号表
    // （1 + 2*128），再留 4 倍余量（匹配+偏移+扩展的场景）。
    if (srcSize > kLZ3MaxBlockSize) return 0;
    return srcSize * 4 + 512;
}

std::vector<uint8_t> compress(const uint8_t* src, size_t srcSize, int level) {
    if (src == nullptr && srcSize != 0) return {};
    if (srcSize == 0) {
        // 空输入：空帧（magic + 0 块）
        return {kLZ3FrameMagic[0], kLZ3FrameMagic[1], kLZ3FrameMagic[2], kLZ3FrameMagic[3],
                0, 0, 0, 0};
    }

    // 统一输出帧格式（magic + 块数 + 每块长度 + 块数据），
    // 便于 decompress() 自动识别与解压。
    const size_t numBlocks = (srcSize + kLZ3MaxBlockSize - 1) / kLZ3MaxBlockSize;
    std::vector<uint8_t> out;
    out.reserve(srcSize + numBlocks * 24 + 8);
    out.insert(out.end(), kLZ3FrameMagic, kLZ3FrameMagic + 4);
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((numBlocks >> shift) & 0xff));
    }
    for (size_t i = 0; i < numBlocks; ++i) {
        const size_t len = std::min(kLZ3MaxBlockSize, srcSize - i * kLZ3MaxBlockSize);
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<uint8_t>((len >> shift) & 0xff));
        }
    }
    for (size_t i = 0; i < numBlocks; ++i) {
        const size_t off = i * kLZ3MaxBlockSize;
        const size_t len = std::min(kLZ3MaxBlockSize, srcSize - off);
        std::vector<uint8_t> block(maxCompressedSizeBlock(len));
        const size_t n = compressBlock(src + off, len, block.data(), block.size(), level);
        if (n == 0) return {};
        block.resize(n);
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

std::vector<uint8_t> decompress(const uint8_t* src, size_t srcSize,
                                size_t rawExpectedSize) {
    if (src == nullptr || srcSize == 0) return {};

    const bool isFrame =
        srcSize >= 8 && src[0] == kLZ3FrameMagic[0] && src[1] == kLZ3FrameMagic[1] &&
        src[2] == kLZ3FrameMagic[2] && src[3] == kLZ3FrameMagic[3];
    if (isFrame) {
        uint32_t numBlocks = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            numBlocks |= static_cast<uint32_t>(src[4 + shift / 8]) << shift;
        }
        size_t p = 8;
        if (numBlocks > srcSize) return {};
        std::vector<uint32_t> sizes(numBlocks);
        size_t total = 0;
        for (size_t i = 0; i < numBlocks; ++i) {
            if (p + 4 > srcSize) return {};
            uint32_t len = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                len |= static_cast<uint32_t>(src[p + shift / 8]) << shift;
            }
            p += 4;
            sizes[i] = len;
            total += len;
        }
        std::vector<uint8_t> out(total);
        size_t o = 0;
        for (size_t i = 0; i < numBlocks; ++i) {
            size_t consumed = 0;
            const size_t n =
                decompressBlock(src + p, srcSize - p, out.data() + o, sizes[i], &consumed);
            if (n == 0 || n != sizes[i] || consumed == 0) return {};
            p += consumed;
            o += n;
        }
        return out;
    }

    // 原始单块格式
    if (rawExpectedSize == 0) return {};
    std::vector<uint8_t> out(rawExpectedSize);
    const size_t n = decompressBlock(src, srcSize, out.data(), rawExpectedSize, nullptr);
    if (n == 0) return {};
    return out;
}

}  // namespace qtsvfs_lz3
