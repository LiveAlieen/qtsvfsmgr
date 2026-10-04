#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qtsvfs {

// 资源目录节点（catalog）里的一条成员声明。
//
// 依据（实测两个独立 catalog：包 0 节点 C8742542E9915DD0 的 923 条、包 101 节点
// C101EAC293405E50 的 31 条，字段布局完全一致）：成员以「[u32 长度][裸 ASCII]」定长
// 字段串成一条记录，同一条记录里先出现资源真名（不含 '/'，如 UX_Glow_CPmj_zzy_002），
// 往后 124~144 字节出现它的资源流路径（assets/35/35b98013….resS）。
// 路径只是「声明」，真才是「名字」：两者配成对之后，路径继续走 hash64('/'+小写)
// == 节点键 的唯一闸门，闸门过了才敢把真名挂到那个节点上。
struct CatalogEntry {
    std::string name;  // 资源真名（不含扩展名）
    std::string path;  // 目录里声明的资源路径（用来定位节点键）
    std::string ext;   // 声明路径的扩展名，小写
};

// 扫明文里的「[u32 长度][字符串]」字段，配对 name↔path。data/size 可以是整块解压结果。
std::vector<CatalogEntry> parseCatalog(const std::uint8_t* data, std::size_t size);

// 单资源节点的自声明名字：这类节点自己就是一个资源的序列化数据，体内只有一个「名字风格」
// 长度前缀字段（实测包 101 的 1240 个无名节点里 985 个恰好 1 个），紧跟其后的是它引用的
// 资源流路径。名字来自「就在这个节点体内」这件事，所以不走哈希闸门；目录段取自节点
// 自己声明的那条路径，属于同一份证据，不是我们编的挂载点。
// 出现 0 个或 ≥3 个候选（聚合 bundle、动画/音频多对象）时返回 false，交给上一层按哈希退化。
struct SelfName {
    std::string name;  // 体内声明的资源名（原样，含空格）
    std::string dir;   // 节点声明的资源流路径的目录，形如 assets/29；没有则空
    bool confident;    // true=体内只有这一个名字，字段就是资源名本身
};

// confident 判定：体内只有 1 个（或 2 个相同）名字字段 → 直接用。
// 不 confident 时（包 8 那种 34 万个小资源文件，体内除了资源名还带着色器属性/子对象名）
// 取「最像资源名」的那个：按 长度 + 2×下划线个数 打分取胜者，且要求体内声明的路径不超过 1 条
// ——路径一大串就说明这是聚合 bundle，取哪个名字都不对，交给上层按哈希退化。
bool parseSelfName(const std::uint8_t* data, std::size_t size, SelfName& out);

}  // namespace qtsvfs
