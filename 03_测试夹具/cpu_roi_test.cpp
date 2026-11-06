// cpu_roi_test.cpp —— surface3RgbBand 的 ROI 裁剪版 vs 整帧版，逐字节对拍 + 计时。
//
// 判据有两条，缺一不可：
//   1) 同一个 src / mask / 参数下，roi=掩码包围盒 与 roi=整帧 的输出必须
//      **逐字节相同**。整帧分支的代码路径与 1.0.5.0 完全一致（crop 退化成整帧），
//      所以这条等价于「裁剪没有改变任何一个像素」。
//   2) 包围盒之外的输出必须等于 src。裁剪版的那些像素是直接 memcpy 的，
//      而它们本该 mask=0 → dst=src；这条防的是「抄漏一块」。
//
// 为什么这条等价关系成立：buildMaskInto 先把掩码清零，再只在人脸框/脖子通道里
// 落笔，包围盒就是落笔像素的边界，所以掩码非零像素必然在包围盒内。
//
// 编译（在本目录下；MSVC 环境变量按 ../01_ofx插件源码/build.ps1 的拼法手工拼，
// 因为沙箱里 vcvars64.bat 会调 reg.exe 被拦）：
//   cl /nologo /O2 /EHsc /MD /utf-8 /std:c++17 /I..\01_ofx插件源码\src ^
//      cpu_roi_test.cpp ..\01_ofx插件源码\src\beauty_smooth.cpp ^
//      ..\01_ofx插件源码\src\beauty_pool.cpp /Fe:cpu_roi_test.exe
// 或直接跑同目录的 build_roi.ps1（推荐的路径，参数与生产构建一致）。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "beauty_smooth.h"

using namespace beautycore;

namespace {

struct Frame {
    int w = 0, h = 0;
    std::vector<unsigned char> px;    // RGBA8
};

// 造一张「有低频渐变 + 有中高频纹理」的图。逐字节对拍不关心内容像不像人脸，
// 但要有足够的高频，否则振幅门控那一支（gw）永远在 1 附近，测不到差异。
Frame makeFrame(int w, int h) {
    Frame f;
    f.w = w;
    f.h = h;
    f.px.resize((size_t)w * h * 4);
    unsigned int seed = 12345u;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            seed = seed * 1664525u + 1013904223u;
            const int noise = (int)((seed >> 16) & 0x1F) - 16;
            const float gx = 130.0f + 60.0f * ((float)x / (float)w);
            const float gy = 110.0f + 50.0f * ((float)y / (float)h);
            const float tex = 12.0f * std::sin((float)x * 0.41f) *
                              std::cos((float)y * 0.37f);
            unsigned char* p = &f.px[((size_t)y * w + x) * 4];
            auto clamp8 = [](float v) {
                int i = (int)(v + 0.5f);
                return (unsigned char)(i < 0 ? 0 : (i > 255 ? 255 : i));
            };
            p[0] = clamp8(gx + tex + (float)noise);
            p[1] = clamp8(gy + tex * 0.8f + (float)noise);
            p[2] = clamp8(120.0f + tex * 0.6f + (float)noise);
            p[3] = 255;
        }
    }
    return f;
}

// 椭圆软边掩码，顺便返回它自己的包围盒（与 buildMaskInto 的口径一致：
// 只统计真的落了非零值的像素）。
RoiRect makeMask(const Frame& f, float cxr, float cyr, float rxr, float ryr,
                 std::vector<unsigned char>& mask) {
    mask.assign((size_t)f.w * f.h, 0);
    const float cx = cxr * (float)f.w, cy = cyr * (float)f.h;
    const float rx = rxr * (float)f.w, ry = ryr * (float)f.h;
    RoiRect bb;
    int bx0 = f.w, by0 = f.h, bx1 = -1, by1 = -1;
    for (int y = 0; y < f.h; y++) {
        for (int x = 0; x < f.w; x++) {
            const float dx = ((float)x + 0.5f - cx) / rx;
            const float dy = ((float)y + 0.5f - cy) / ry;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= 1.0f) continue;
            // 边缘 15% 做软过渡，模拟羽化
            float t = (d > 0.85f) ? (1.0f - d) / 0.15f : 1.0f;
            if (t <= 0.0f) continue;
            const int v = (int)(t * 255.0f + 0.5f);
            if (v <= 0) continue;
            mask[(size_t)y * f.w + x] = (unsigned char)v;
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    }
    if (bx1 < 0) return bb;
    bb.x0 = bx0;
    bb.y0 = by0;
    bb.x1 = bx1 + 1;
    bb.y1 = by1 + 1;
    return bb;
}

int countDiff(const std::vector<unsigned char>& a,
              const std::vector<unsigned char>& b, size_t* firstAt) {
    int n = 0;
    *firstAt = (size_t)-1;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) {
        if (a[i] != b[i]) {
            if (*firstAt == (size_t)-1) *firstAt = i;
            n++;
        }
    }
    return n;
}

double timeIt(const std::function<void()>& fn, int reps) {
    // 取中位数，避开第一次的冷缓存
    std::vector<double> t;
    for (int i = 0; i < reps; i++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        fn();
        auto t1 = std::chrono::high_resolution_clock::now();
        t.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

int runCase(const char* tag, int w, int h, float cx, float cy, float rx, float ry,
            bool halfRes, int passes, float I, int threads, bool verbose) {
    Frame f = makeFrame(w, h);
    std::vector<unsigned char> mask;
    const RoiRect bb = makeMask(f, cx, cy, rx, ry, mask);
    if (bb.empty()) {
        printf("  [%s] 掩码为空，跳过\n", tag);
        return 1;
    }
    std::vector<unsigned char> pr((size_t)w * h, 0);
    const RoiRect full{0, 0, w, h};

    BandParams p;
    p.halfRes = halfRes ? 1 : 0;
    p.passes = passes;
    p.I = I;

    std::vector<unsigned char> dstRoi((size_t)w * h * 4, 0xAB);
    std::vector<unsigned char> dstFull((size_t)w * h * 4, 0xCD);
    std::vector<float> scA, scB;

    auto runRoi = [&]() {
        surface3RgbBand(&f.px[0], &dstRoi[0], w, h, &mask[0], &pr[0], p, bb,
                        threads, scA);
    };
    auto runFull = [&]() {
        surface3RgbBand(&f.px[0], &dstFull[0], w, h, &mask[0], &pr[0], p, full,
                        threads, scB);
    };
    runRoi();
    runFull();

    size_t firstAt = 0;
    const int nd = countDiff(dstRoi, dstFull, &firstAt);

    // 包围盒之外必须原样等于 src
    int outBad = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (x >= bb.x0 && x < bb.x1 && y >= bb.y0 && y < bb.y1) continue;
            const size_t o = ((size_t)y * w + x) * 4;
            if (memcmp(&dstRoi[o], &f.px[o], 4) != 0) outBad++;
        }
    }

    const double tRoi = timeIt(runRoi, 5);
    const double tFull = timeIt(runFull, 5);
    const double areaRoi = (double)bb.area();
    const double areaAll = (double)w * h;

    const char* verdict = (nd == 0 && outBad == 0) ? "PASS" : "FAIL";
    printf("  [%-3s] %4dx%-4d half=%d P=%d I=%.1f roi=%dx%d(%.0f%%)  "
           "逐字节差异=%d  框外非src=%d  裁剪 %.2fms / 整帧 %.2fms  %.2fx\n",
           verdict, w, h, halfRes ? 1 : 0, passes, I,
           bb.width(), bb.height(), 100.0 * areaRoi / areaAll,
           nd, outBad, tRoi, tFull, tFull / (tRoi > 1e-6 ? tRoi : 1e-6));
    if (verbose && nd) {
        printf("        首个差异字节 offset=%zu (像素 %zu, 通道 %zu)\n", firstAt,
               firstAt / 4, firstAt % 4);
    }
    return (nd == 0 && outBad == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const int threads = (argc > 1) ? atoi(argv[1]) : 8;
    printf("=== surface3RgbBand ROI 裁剪 vs 整帧（线程=%d）===\n", threads);
    int fail = 0;

    struct Sz { int w, h; };
    const Sz sizes[] = {{1920, 1080}, {3840, 2160}, {640, 394}, {1080, 1920}};

    for (const Sz& s : sizes) {
        // 典型人脸框：画面中部、约占短边 45%
        fail += runCase("中心", s.w, s.h, 0.5f, 0.42f, 0.16f, 0.28f,
                        true, 3, 0.8f, threads, true);
        // 贴左上角（裁剪被画面边界截断的分支）
        fail += runCase("贴角", s.w, s.h, 0.10f, 0.10f, 0.09f, 0.16f,
                        true, 3, 0.8f, threads, true);
        // 贴右边缘
        fail += runCase("贴边", s.w, s.h, 0.93f, 0.50f, 0.07f, 0.22f,
                        true, 3, 0.8f, threads, true);
        // 大脸（占大半画面，验证「裁剪没收益时也不能出错」）
        fail += runCase("大脸", s.w, s.h, 0.5f, 0.5f, 0.42f, 0.44f,
                        true, 3, 0.8f, threads, true);
    }
    // 参数组合覆盖：全分辨率平面 / 不同 passes / 满强度
    fail += runCase("无半分辨率", 1920, 1080, 0.5f, 0.45f, 0.18f, 0.30f, false, 3, 0.8f, threads, true);
    fail += runCase("passes=1", 1920, 1080, 0.5f, 0.45f, 0.18f, 0.30f, true, 1, 0.8f, threads, true);
    fail += runCase("passes=2", 3840, 2160, 0.5f, 0.45f, 0.18f, 0.30f, true, 2, 1.0f, threads, true);
    fail += runCase("I=1.0", 1920, 1080, 0.5f, 0.45f, 0.18f, 0.30f, true, 3, 1.0f, threads, true);
    fail += runCase("单线程", 1920, 1080, 0.5f, 0.45f, 0.18f, 0.30f, true, 3, 0.8f, 1, true);

    printf("\n%s（失败 %d 项）\n", fail == 0 ? "全部 PASS" : "存在 FAIL", fail);
    return fail == 0 ? 0 : 1;
}
