// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Jian
// hwprobe_test.cpp —— beauty_hwprobe 的离线验收程序
// 编译：cl /O2 /EHsc /MD /std:c++17 /utf-8 hwprobe_test.cpp beauty_hwprobe.cpp
#include <windows.h>
#include <cstdio>
#include "beauty_hwprobe.h"

int main() {
    SetConsoleOutputCP(CP_UTF8);
    char buf[2048];

    printf("=== Phase 1: static probe ===\n");
    beautyHwDescribe(buf, sizeof(buf));
    printf("%s\n", buf);

    printf("=== Phase 2: bandwidth calibration ===\n");
    long long t0 = GetTickCount64();
    bool ok = beautyHwCalibrate();
    long long dt = (long long)(GetTickCount64() - t0);
    printf("calibrate=%d  took=%lld ms\n\n", (int)ok, dt);

    beautyHwDescribe(buf, sizeof(buf));
    printf("%s\n", buf);

    printf("=== Phase 3: path decisions (AUTO) ===\n");
    struct { int w, h; const char* n; } cases[] = {
        { 3840, 2160, "4K" },
        { 1920, 1080, "1080p" },
        { 1280,  720, "720p" },
        {  640,  394, "preview" },
        {  280,  158, "thumb" },
    };
    for (auto& c : cases) {
        BeautyPath p = beautyChoosePath(c.w, c.h, BEAUTY_PATH_AUTO);
        printf("  %-8s %4dx%-4d -> %s\n", c.n, c.w, c.h,
               p == BEAUTY_PATH_GPU ? "GPU" : "CPU");
    }

    printf("\n=== Phase 4: forced overrides ===\n");
    printf("  forced GPU on thumb  -> %s\n",
           beautyChoosePath(280, 158, BEAUTY_PATH_GPU) == BEAUTY_PATH_GPU ? "GPU" : "CPU");
    printf("  forced CPU on 4K     -> %s\n",
           beautyChoosePath(3840, 2160, BEAUTY_PATH_CPU) == BEAUTY_PATH_GPU ? "GPU" : "CPU");

    printf("\ncpuThreads=%d\n", beautyHwCpuThreads());
    beautyHwOneLine(buf, sizeof(buf));
    printf("oneLine: %s\n", buf);
    return 0;
}
