// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Jian
// gpu_pipe_test.cpp —— P1 管道离线验收（RGBA8 口径）
//
// 验四件事：
//   1) GPU_BEAUTY_IDENTITY 在 1080p / 4K、全帧 / 人脸 ROI 下逐字节无损
//   2) GPU_BEAUTY_SWAP_RB 确实产生差异 —— 证明 CS 真的执行了，且校验函数
//      抓得住差异（否则「恒等全过」可能只是根本没跑）
//   3) 各场景的上传 / 提交 / 读回耗时与有效带宽
//   4) ROI 相对全帧的实际收益

#include "gpu_beauty.h"
#include "beauty_hwprobe.h"
#include "beauty_log.h"

#include <cstdio>
#include <cstring>
#include <vector>

static void fillPattern(std::vector<unsigned char>& buf, int w, int h) {
    // 造一张取值覆盖全 0..255、含锐利边缘与渐变的图：
    // 恒等拷贝若被任何一步「优化」成有损转换，这里立刻暴露。
    buf.resize((size_t)w * h * 4);
    unsigned int s = 0x12345678u;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            s = s * 1664525u + 1013904223u;
            unsigned char* p = &buf[((size_t)y * w + x) * 4];
            p[0] = (unsigned char)(s >> 24);
            p[1] = (unsigned char)(x * 255 / (w > 1 ? w - 1 : 1));
            p[2] = (unsigned char)(y * 255 / (h > 1 ? h - 1 : 1));
            p[3] = ((x / 64 + y / 64) & 1) ? 255 : 64;
        }
    }
}

int main() {
    if (!gpuBeautyInit()) {
        std::printf("初始化失败: %s\n", gpuBeautyLastError());
        return 1;
    }

    struct Case { const char* name; int w, h; int rx0, ry0, rx1, ry1; };
    const Case cases[] = {
        {"1080p 全帧", 1920, 1080, 0, 0, 1920, 1080},
        {"1080p 人脸", 1920, 1080, 480, 140, 1440, 940},
        {"4K 全帧",    3840, 2160, 0, 0, 3840, 2160},
        {"4K 人脸",    3840, 2160, 1280, 360, 2560, 1800},
    };

    std::printf("%-12s %-11s %-8s %-7s %-7s %-7s %-7s %s\n",
                "场景", "ROI", "像素", "上传", "提交", "读回", "合计", "一致性");
    std::printf("------------------------------------------------------------------------------------\n");

    for (const Case& c : cases) {
        std::vector<unsigned char> buf;
        fillPattern(buf, c.w, c.h);

        GpuBeautyRoi roi{c.rx0, c.ry0, c.rx1, c.ry1};
        const int runs = 16;
        GpuBeautyTiming t{};
        int mism = -1, maxd = 0;
        double sum[3] = {0, 0, 0};
        bool ok = true;

        for (int i = 0; i < runs; ++i) {
            std::memset(&t, 0, sizeof(t));
            ok = gpuBeautyVerifyRoi(&buf[0], c.w, c.h, c.w, roi,
                                    GPU_BEAUTY_IDENTITY, &t, &mism, &maxd) && ok;
            if (i > 0) { sum[0] += t.uploadMs; sum[1] += t.submitMs; sum[2] += t.downloadMs; }
        }
        const double k = 1.0 / (runs - 1);
        const double mpx = (double)(c.rx1 - c.rx0) * (c.ry1 - c.ry0) / 1e6;
        std::printf("%-12s %4dx%-6d %6.2fMP %6.2f  %6.2f  %6.2f  %6.2f  ",
                    c.name, c.rx1 - c.rx0, c.ry1 - c.ry0, mpx,
                    sum[0] * k, sum[1] * k, sum[2] * k, (sum[0] + sum[1] + sum[2]) * k);
        if (!ok) std::printf("不一致 %d 字节 max=%d\n", mism, maxd);
        else     std::printf("逐字节一致\n");
    }

    // 反证：SWAP_RB 必须被校验函数判为「不一致」，否则说明 CS 没跑或比对失效
    {
        std::vector<unsigned char> buf;
        fillPattern(buf, 1920, 1080);
        GpuBeautyRoi roi{0, 0, 1920, 1080};
        GpuBeautyTiming t{};
        int mism = -1, maxd = 0;
        const bool same = gpuBeautyVerifyRoi(&buf[0], 1920, 1080, 1920, roi,
                                             GPU_BEAUTY_SWAP_RB, &t, &mism, &maxd);
        std::printf("\n反证 SWAP_RB：判定一致=%s（应为否），不一致 %d 字节，max=%d\n",
                    same ? "是" : "否", mism, maxd);
        if (same) std::printf("  ==> CS 没执行或比对失效，管道不可信\n");
    }

    {
        long long n; double au, ag, ad; int lm, lmd;
        gpuBeautyStats(&n, &au, &ag, &ad, &lm, &lmd);
        std::printf("累计 %lld 帧，均 上传=%.2f 提交=%.2f 读回=%.2f ms，最后一次不一致=%d\n",
                    n, au, ag, ad, lm);
    }

    gpuBeautyShutdown();
    return 0;
}
