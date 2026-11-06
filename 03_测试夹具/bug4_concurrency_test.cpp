// bug4_concurrency_test.cpp —— 双线程并发跑 CoreEngine，验证 slot 状态不串台。
//
// 判据：同一个 slot 在同一串输入上，「单线程顺序跑」与「两线程并发跑」的逐帧
// 输出必须完全相同。修复前 SlotScope 无锁，A 的 mask_/heldFrame_ 会被 B 换走，
// 逐帧哈希必然分叉，最坏直接访问越界。
//
// 2026-11-06 实测（RTX 5060 Laptop，960x540，20 帧）：
//   无锁版：A/B 都在**第 0 帧**就分叉 → FAIL
//   加锁版：A/B 各 20 帧逐字节一致 → PASS
// 所以这个用例确实有分辨力，不是恒过的摆设。
//
// 为什么要单独链接成 EXE：C 接口（core_glue.h）没有从 .ofx 导出（没有
// __declspec(dllexport)，也没有 .def），所以 ctypes 摸不到 —— 只能把同一批
// 源文件链接进一个 EXE 直接调。
//
// 用法：
//   1) 准备输入（两路内容不同、但都能检出同一张脸）：
//        python -c "from PIL import Image; import numpy as np; \
//          w,h=960,540; a=np.asarray(Image.open('04_验收工具/input_face.png')\
//          .convert('RGBA').resize((w,h),Image.LANCZOS),dtype=np.uint8); \
//          a.tofile('face_A.raw'); np.clip((np.power(a.astype(np.float32)/255.,1.35)*255.)\
//          [:,:[2,1,0,3]],0,255).astype(np.uint8).tofile('face_B.raw')"
//   2) 把 src\*.cpp + 本文件一起编译（/utf-8 必须加，否则中文注释会触发 C4819），
//      链接 d3d11/dxgi/d3dcompiler/advapi32/opengl32/gdi32/user32。
//   3) 把 onnxruntime.dll 与 yunet.onnx 放到 EXE 同级，然后：
//        bug4_test.exe <引擎目录> [帧数]
//
// 注意：必须 coreBeautySetAsyncDetect(0)，否则掩码来自 worker，同一输入两次
// 跑出来的结果本来就允许不同，用例会假失败。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core_glue.h"

static const int W = 960;
static const int H = 540;
static const int P_COUNT = 14;

static bool loadRaw(const char* path, std::vector<unsigned char>& v) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    v.resize((size_t)W * H * 4);
    const size_t n = fread(&v[0], 1, v.size(), f);
    fclose(f);
    return n == v.size();
}

static unsigned long long fnv(const std::vector<unsigned char>& v) {
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < v.size(); ++i) { h ^= v[i]; h *= 1099511628211ULL; }
    return h;
}

static void runFrames(int slot, const std::vector<unsigned char>& src, int frames,
                      std::vector<unsigned long long>& out) {
    double panel[P_COUNT] = {0};
    panel[0] = 50.0;   // 磨皮
    panel[2] = 30.0;   // 美白
    panel[3] = 30.0;   // 红润
    std::vector<unsigned char> dst(src.size());
    for (int i = 0; i < frames; ++i) {
        const bool ok = coreBeautyProcess(slot, &src[0], &dst[0], W, H,
                                          panel, 0, 0, 0, true);
        out.push_back(ok ? fnv(dst) : 0ULL);
    }
}

int main(int argc, char** argv) {
    const char* engineDir = (argc > 1) ? argv[1] : ".";
    const int frames = (argc > 2) ? atoi(argv[2]) : 20;

    std::vector<unsigned char> fa, fb;
    if (!loadRaw("face_A.raw", fa) || !loadRaw("face_B.raw", fb)) {
        printf("缺 face_A.raw / face_B.raw（960x540 RGBA）\n");
        return 2;
    }

    char err[512] = {0};
    if (!coreBeautyEnsureReady(engineDir, err, (int)sizeof(err))) {
        printf("引擎初始化失败：%s\n", err);
        return 2;
    }
    coreBeautySetAsyncDetect(0);     // 确定性：不要 worker 的异步掩码
    coreBeautySetImageBottomUp(0);   // 夹具是 top-down

    // ---- 阶段 1：单线程基线 ----
    int sa = coreBeautyAcquireSlot(err, (int)sizeof(err));
    int sb = coreBeautyAcquireSlot(err, (int)sizeof(err));
    if (sa < 0 || sb < 0) { printf("acquireSlot 失败：%s\n", err); return 2; }

    std::vector<unsigned long long> refA, refB;
    runFrames(sa, fa, frames, refA);
    runFrames(sb, fb, frames, refB);
    coreBeautyReleaseSlot(sa);
    coreBeautyReleaseSlot(sb);
    printf("基线 A 末帧=%016llX  B 末帧=%016llX\n", refA.back(), refB.back());

    // ---- 阶段 2：两线程并发 ----
    sa = coreBeautyAcquireSlot(err, (int)sizeof(err));
    sb = coreBeautyAcquireSlot(err, (int)sizeof(err));
    std::vector<unsigned long long> gotA, gotB;
    std::thread ta(runFrames, sa, std::cref(fa), frames, std::ref(gotA));
    std::thread tb(runFrames, sb, std::cref(fb), frames, std::ref(gotB));
    ta.join();
    tb.join();
    coreBeautyReleaseSlot(sa);
    coreBeautyReleaseSlot(sb);

    int bad = 0;
    const char* names[2] = {"A", "B"};
    std::vector<unsigned long long>* refs[2] = {&refA, &refB};
    std::vector<unsigned long long>* gots[2] = {&gotA, &gotB};
    for (int k = 0; k < 2; ++k) {
        if (refs[k]->size() != gots[k]->size()) { printf("[FAIL] %s 帧数不符\n", names[k]); ++bad; continue; }
        int diff = -1;
        for (size_t i = 0; i < refs[k]->size(); ++i)
            if ((*refs[k])[i] != (*gots[k])[i]) { diff = (int)i; break; }
        if (diff < 0) printf("[PASS] %s 路 %d 帧与单线程基线逐帧一致\n", names[k], frames);
        else { printf("[FAIL] %s 第 %d 帧不一致 ref=%016llX got=%016llX\n",
                      names[k], diff, (*refs[k])[diff], (*gots[k])[diff]); ++bad; }
    }

    coreBeautyRelease();
    printf("结论：%s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}
