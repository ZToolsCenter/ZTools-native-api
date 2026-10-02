// 截图翻译算法层 C ABI shim 实现（translate_cluster 的 macOS 桥）。
// 仅做参数打包/解包与内存分配转发，业务逻辑全部在 algo/translate_cluster.cpp。
// 仅 macOS 构建链编译（build-swift.sh / CI）。
#include "translate_bridge_mac.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "../algo/translate_cluster_internal.h"

int32_t tc_abi_version(void) {
    return TC_ABI_VERSION;
}

int32_t tc_cluster_lines(const TcLineC* lines, int32_t lineCount,
                         TcClusterResultC* out) {
    if (out == nullptr || (lines == nullptr && lineCount > 0) || lineCount < 0) {
        return 0;
    }
    std::vector<TcLine> input;
    input.reserve(static_cast<size_t>(lineCount));
    for (int32_t i = 0; i < lineCount; i++) {
        TcLine tl;
        tl.text = lines[i].text ? lines[i].text : "";
        tl.x = lines[i].x;
        tl.y = lines[i].y;
        tl.w = lines[i].w;
        tl.h = lines[i].h;
        input.push_back(std::move(tl));
    }
    TcClusterResult clustered = TcClusterLines(input);

    // 输出数组一次性 calloc，成员字符串/下标数组逐段分配；任一分配失败整体回滚，
    // 保证 out 要么完整可用、要么原样清零（无部分填充的悬垂指针）。
    TcClusterResultC result = {};
    result.paragraphCount = static_cast<int32_t>(clustered.paragraphs.size());
    result.medianLineH = clustered.medianLineH;
    result.medianPitch = clustered.medianPitch;
    result.boundaryCount = clustered.boundaryCount;
    if (!clustered.paragraphs.empty()) {
        TcParagraphC* paras = static_cast<TcParagraphC*>(
            calloc(clustered.paragraphs.size(), sizeof(TcParagraphC)));
        if (paras == nullptr) {
            return 0;
        }
        bool ok = true;
        for (size_t i = 0; i < clustered.paragraphs.size() && ok; i++) {
            const TcParagraph& p = clustered.paragraphs[i];
            char* text = static_cast<char*>(calloc(p.text.size() + 1, 1));
            int32_t* idx = static_cast<int32_t*>(
                calloc(p.lineIdx.size() > 0 ? p.lineIdx.size() : 1, sizeof(int32_t)));
            if (text == nullptr || idx == nullptr) {
                free(text);
                free(idx);
                ok = false;
                break;
            }
            memcpy(text, p.text.c_str(), p.text.size());
            for (size_t k = 0; k < p.lineIdx.size(); k++) {
                idx[k] = p.lineIdx[k];
            }
            paras[i].x = p.x;
            paras[i].y = p.y;
            paras[i].w = p.w;
            paras[i].h = p.h;
            paras[i].lineHeight = p.lineHeight;
            paras[i].text = text;
            paras[i].lineIdx = idx;
            paras[i].lineCount = static_cast<int32_t>(p.lineIdx.size());
        }
        if (!ok) {
            for (size_t i = 0; i < clustered.paragraphs.size(); i++) {
                free(paras[i].text);
                free(paras[i].lineIdx);
            }
            free(paras);
            return 0;
        }
        result.paragraphs = paras;
    }
    *out = result;
    return 1;
}

void tc_cluster_free_result(TcClusterResultC* result) {
    if (result == nullptr) return;
    if (result->paragraphs != nullptr) {
        for (int32_t i = 0; i < result->paragraphCount; i++) {
            free(result->paragraphs[i].text);
            free(result->paragraphs[i].lineIdx);
        }
        free(result->paragraphs);
    }
    result->paragraphs = nullptr;
    result->paragraphCount = 0;
    result->medianLineH = 0;
    result->medianPitch = 0;
    result->boundaryCount = 0;
}
