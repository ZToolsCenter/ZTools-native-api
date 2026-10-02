// 截图翻译算法层 C ABI shim（translate_cluster 的 macOS 桥）。
//
// 定位：把 algo/translate_cluster.cpp（纯算法层，只依赖 C++ 标准库）的
// TcClusterLines 打包为固定布局的 C 接口，供 macOS 侧 Swift（经 @_silgen_name）
// 链入 libZToolsNative.dylib 调用。本层不含任何业务逻辑——只做参数打包/解包
// 转发，纪律与 lc_bridge_mac.h 一致：结构体全部固定宽度标量、字段顺序即内存
// 布局（自然对齐）、Swift 侧按同序镜像。仅在 macOS 构建链（build-swift.sh /
// CI）编译；Windows 侧直接编入 .node（binding.gyp win 分支），不包含本文件。
//
// 内存纪律：
//   · 输入 TcLineC.text 指针由调用方持有，仅在 tc_cluster_lines 调用期间有效；
//   · 输出 TcClusterResultC 内的 paragraphs/lineIdx/text 均由本层 calloc 分配，
//     调用方用毕必须以 tc_cluster_free_result 整体释放（free 单个字段等价，
//     均为 malloc 家族分配）；tc_cluster_lines 对 out 中残留指针先做防御性释放。
#ifndef TRANSLATE_BRIDGE_MAC_H
#define TRANSLATE_BRIDGE_MAC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI 版本（结构体布局或语义变更时递增；Swift 侧启动时可校验）。
#define TC_ABI_VERSION 1

// 聚类输入：一行 OCR 文本（提交图像内像素坐标；镜像 algo 的 TcLine，
// text 为 UTF-8 C 字符串，调用期间有效）。
typedef struct TcLineC {
    const char* text;
    double x;
    double y;
    double w;
    double h;
} TcLineC;

// 聚类产出的一个段落（镜像 TcParagraph；lineIdx 为成员行下标数组，
// text/lineIdx 由本层分配，随 tc_cluster_free_result 释放）。
typedef struct TcParagraphC {
    double x;               // 成员行包围盒并集（提交图像内像素坐标）
    double y;
    double w;
    double h;
    double lineHeight;      // 段落原文行高估计（成员行高中位数）
    char* text;             // 成员行文本按 CJK/拉丁规则拼接的结果（UTF-8）
    int32_t* lineIdx;       // 成员行下标（输入 lines 的原始下标，按阅读序）
    int32_t lineCount;      // 成员行数（lineIdx 长度；版本面特征统计用）
} TcParagraphC;

// 聚类结果 + 全局统计（镜像 TcClusterResult）。
typedef struct TcClusterResultC {
    int32_t paragraphCount;   // paragraphs 数组长度
    TcParagraphC* paragraphs; // 段落数组（本层分配）
    double medianLineH;       // 页面正常行高估计（全部行高中位数）
    double medianPitch;       // 页面正常行距估计
    int32_t boundaryCount;    // 判为段落边界的相邻对数量
} TcClusterResultC;

// ABI 版本查询（返回 TC_ABI_VERSION）。
int32_t tc_abi_version(void);

// 主入口：OCR 文本行数组 → 段落数组（转发 TcClusterLines 纯函数）。
// 返回 1 成功（out 已填充）/ 0 参数非法（out 不变）。
int32_t tc_cluster_lines(const TcLineC* lines, int32_t lineCount,
                         TcClusterResultC* out);

// 释放 tc_cluster_lines 产出的结果（对已清零/从未填充的 out 安全 no-op）。
void tc_cluster_free_result(TcClusterResultC* result);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TRANSLATE_BRIDGE_MAC_H
