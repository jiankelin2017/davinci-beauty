// beauty_face.cpp —— YuNet 人脸检测实现（前处理 + 推理 + 解码 + NMS）
//
// 解码公式是从实测反推的（640×640 上 cv2.FaceDetectorYN 当真值校准）：
//     cx = (col + bx) * stride
//     cy = (row + by) * stride
//     w  = exp(bw) * stride
//     h  = exp(bh) * stride
//     score = sqrt(cls * obj)
// 校验依据：stride=32 上最佳 anchor 解出 cx≈327.9/cy≈278.7、w≈109.9，
// 与 cv2 的 324.2/276.0、111.3 相差不到 3%。

#include "beauty_face.h"

#include "beauty_onnx.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace beautycore {

namespace {

// 默认推理边长。实际值放在 FaceDetector::inputSize_ 里（可运行期调整）。
const int kDefaultInputSize = 640;

// 推理线程数。YuNet 640x640 单线程约 15 ms，是整条链最贵的一步；
// 给 2 个线程就能砍掉近一半，且不会把 OBS 的编码线程挤掉（编码通常占 4~6 核）。
const int kInferThreads = 2;
const int kStrides[3] = {8, 16, 32};

double nowMs() {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// v3.0.2 诊断：BEAUTY_DEBUG_DETECT=1 时把每帧检测关键数据追加到日志文件，
// 用于定位「score=0.00（模型没产出分数）」这类问题。默认关闭，零性能影响。
bool dbgDetectOn() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("BEAUTY_DEBUG_DETECT"); v = (e && *e && e[0] != '0') ? 1 : 0; }
    return v == 1;
}
void dbgDetect(const char* fmt, ...) {
    if (!dbgDetectOn()) return;
    const char* p = getenv("BEAUTY_DEBUG_LOG");
    if (!p || !*p) p = "C:\\ProgramData\\DaVinciBeauty\\logs\\detect_debug.log";
    FILE* f = fopen(p, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// v3.0.2 诊断：BEAUTY_DEBUG_DUMP=<目录> 时，把「真正送进模型的缓冲区」落盘，
// 用于把「输入内容错」和「推理/解码错」彻底分开 —— 把 in.f32 交给 Python 用
// 同一个 onnx 再跑一遍，就能判定到底是哪一段出的问题。默认关闭。
const char* dbgDumpDir() {
    static const char* d = nullptr;
    static bool tried = false;
    if (!tried) { tried = true; const char* e = getenv("BEAUTY_DEBUG_DUMP"); if (e && *e) d = e; }
    return d;
}
bool dbgWrite(const char* name, const void* data, size_t bytes) {
    const char* dir = dbgDumpDir();
    if (!dir || !data || !bytes) return false;
    char path[1024];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(data, 1, bytes, f);
    fclose(f);
    return true;
}

// 等比缩放 + 居中 letterbox 到 640x640。
//
// 为什么不用直接拉伸：OBS 的视频帧多是 16:9，直接拉成正方形会把脸横向压扁
// （变形 1.8 倍），YuNet 的长宽比先验会被破坏。letterbox 保持比例，代价是黑边，
// 与 OpenCV FaceDetectorYN 的默认行为也一致，便于用 cv2 当真值校准。
//
// flipY：源缓冲是 bottom-up（OFX/Resolve 的约定）时按行反读。插值权重仍然
// 属于「视觉行 y1」，所以只把读指针换成 sh-1-y，不需要改权重，也不需要
// 整帧翻转 —— 这里本来就要做一次缩放重采样，反读是零成本的。
void letterboxRgb(const unsigned char* src, int sw, int sh, int stride,
                  unsigned char* dst, int dw, int dh,
                  float& scale, int& ox, int& oy, bool flipY) {
    scale = fminf((float)dw / sw, (float)dh / sh);
    const int nw = (int)(sw * scale + 0.5f);
    const int nh = (int)(sh * scale + 0.5f);
    ox = (dw - nw) / 2;
    oy = (dh - nh) / 2;
    memset(dst, 0, (size_t)dw * dh * 3);
    for (int y = 0; y < nh; y++) {
        const float fy = (y + 0.5f) / scale - 0.5f;
        int y0 = (int)floorf(fy);
        const float wy = fy - y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 >= sh) y1 = sh - 1;
        if (y0 >= sh) y0 = sh - 1;
        // bottom-up：视觉行 r 在缓冲里的行号是 sh-1-r（y0/y1 已夹到 [0,sh-1]）
        const int r0 = flipY ? (sh - 1 - y0) : y0;
        const int r1 = flipY ? (sh - 1 - y1) : y1;
        const unsigned char* r0p = src + (size_t)r0 * sw * stride;
        const unsigned char* r1p = src + (size_t)r1 * sw * stride;
        unsigned char* o = dst + (size_t)(oy + y) * dw * 3 + (size_t)ox * 3;
        for (int x = 0; x < nw; x++) {
            const float fx = (x + 0.5f) / scale - 0.5f;
            int x0 = (int)floorf(fx);
            const float wx = fx - x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 >= sw) x1 = sw - 1;
            if (x0 >= sw) x0 = sw - 1;
            for (int k = 0; k < 3; k++) {
                const float a = r0p[(size_t)x0 * stride + k] * (1 - wx) +
                                r0p[(size_t)x1 * stride + k] * wx;
                const float b = r1p[(size_t)x0 * stride + k] * (1 - wx) +
                                r1p[(size_t)x1 * stride + k] * wx;
                o[(size_t)x * 3 + k] = (unsigned char)(a * (1 - wy) + b * wy + 0.5f);
            }
        }
    }
}

float iouOf(const FaceBox& a, const FaceBox& b) {
    const float ax2 = a.x + a.w, ay2 = a.y + a.h;
    const float bx2 = b.x + b.w, by2 = b.y + b.h;
    const float ix = fmaxf(0.f, fminf(ax2, bx2) - fmaxf(a.x, b.x));
    const float iy = fmaxf(0.f, fminf(ay2, by2) - fmaxf(a.y, b.y));
    const float inter = ix * iy;
    const float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0.f;
}

}  // namespace

bool FaceDetector::loaded() const { return model_ != nullptr; }

void FaceDetector::release() {
    delete (OnnxModel*)model_;
    model_ = nullptr;
    input_.clear();
    resized_.clear();
}

void FaceDetector::setInputSize(int px) {
    if (fixedSize_) return;      // 模型写死了尺寸，改了会让推理直接报错
    int s = px;
    if (s < 160) s = 160;
    if (s > 1024) s = 1024;
    s = (s / 32) * 32;      // stride 8/16/32 都要求整除，向下取整最省事
    if (s < 160) s = 160;
    inputSize_ = s;
}

bool FaceDetector::load(const char* modelPath, std::string& err) {
    release();
    inputSize_ = kDefaultInputSize;   // 动态 shape 时按默认值跑
    fixedSize_ = false;
    OnnxModel* m = new OnnxModel();
    if (!m->load(modelPath, err, kInferThreads)) {
        delete m;
        return false;
    }
    // 形状校验：YuNet 2023mar 导出的是动态 shape（实测 320/480/640 都能跑），
    // 但别家导出的可能是固定 shape —— 那种情况下要跟着模型走，否则推理会报错。
    const std::vector<int64_t>& sh = m->inputShape();
    if (sh.size() != 4 || sh[1] != 3) {
        err = "模型输入不是 1x3xHxW";
        delete m;
        return false;
    }
    if (sh[2] > 0 && sh[3] > 0 && sh[2] == sh[3]) {
        inputSize_ = (int)sh[2];   // 固定 shape：必须用它，不能自己选
        fixedSize_ = true;
    }
    model_ = m;
    provider_ = m->provider();   // 同步执行提供器（CPU / DirectML）

    // v3.0.2：预处理档（见 detect() 注释）。默认 0 = raw 0-255（修复档）。
    // 取值 0..5；非法值保持默认 0。
    preprocMode_ = 0;
    const char* pp = getenv("BEAUTY_YUNET_PREPROC");
    if (pp && *pp) {
        int v = atoi(pp);
        if (v >= 0 && v <= 5) preprocMode_ = v;
    }
    return true;
}

bool FaceDetector::detect(const unsigned char* rgb, int w, int h,
                          std::vector<FaceBox>& faces, std::string& err,
                          float conf, float nms, int stride, bool flipY) {
    faces.clear();
    if (!model_) { err = "检测模型未加载"; return false; }
    if (!rgb || w <= 0 || h <= 0) { err = "输入无效"; return false; }
    const double t0 = nowMs();

    const int S = inputSize_;
    bestScore_ = 0.f;
    resized_.resize((size_t)S * S * 3);
    float scale = 1.f;
    int ox = 0, oy = 0;
    letterboxRgb(rgb, w, h, stride, &resized_[0], S, S, scale, ox, oy, flipY);

    // 诊断落盘：只取第一帧「真实尺寸」（h>360，跳过预热用的 360 行假帧）。
    static int dumpDone = 0;
    const bool wantDump = (dbgDumpDir() != nullptr) && !dumpDone && h > 360;
    if (wantDump) {
        dbgWrite("src.raw", rgb, (size_t)w * h * (size_t)stride);
        dbgWrite("lb.raw", &resized_[0], resized_.size());
    }

    input_.resize((size_t)3 * S * S);
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            const unsigned char* p = &resized_[((size_t)y * S + x) * 3];
            const size_t i = (size_t)y * S + x;
            // v3.0.2 修正：本 yunet.onnx 吃 **raw 0-255**，任何归一化
            // （(x-127.5)/128 或 x/255）都会让检测直接归零 → 引擎降级全帧磨皮。
            // 已用本地模型做实跑验证：raw RGB/BGR 均检出 8 张脸（bestScore≈0.92），
            // 三种归一化档全 0。默认档（preprocMode_==0）按源缓冲原顺序
            // （RGBA/BGRA 保序）喂 0-255，不做任何归一化。通道序对检出无影响
            // （已验证），故不交换。调试档 1..5 仅用于极端导出差异排查：
            //   1 = 显式 BGR raw（源为 RGBA 时）  2/3 = (x-127.5)/128 RGB/BGR
            //   4/5 = x/255 RGB/BGR
            const float r = (float)p[0], g = (float)p[1], b = (float)p[2];
            float c0, c1, c2;
            if (preprocMode_ == 1) {            // 调试：显式 BGR raw
                c0 = b; c1 = g; c2 = r;
            } else if (preprocMode_ == 2) {     // 调试：(x-127.5)/128 RGB
                const float inv = 1.0f / 128.0f;
                c0 = (r - 127.5f) * inv; c1 = (g - 127.5f) * inv; c2 = (b - 127.5f) * inv;
            } else if (preprocMode_ == 3) {     // 调试：(x-127.5)/128 BGR
                const float inv = 1.0f / 128.0f;
                c0 = (b - 127.5f) * inv; c1 = (g - 127.5f) * inv; c2 = (r - 127.5f) * inv;
            } else if (preprocMode_ == 4) {     // 调试：x/255 RGB
                const float inv = 1.0f / 255.0f;
                c0 = r * inv; c1 = g * inv; c2 = b * inv;
            } else if (preprocMode_ == 5) {     // 调试：x/255 BGR
                const float inv = 1.0f / 255.0f;
                c0 = b * inv; c1 = g * inv; c2 = r * inv;
            } else {                            // 默认 0：raw 0-255，源顺序
                c0 = r; c1 = g; c2 = b;
            }
            input_[i]                 = c0;
            input_[i + (size_t)S * S] = c1;
            input_[i + 2 * (size_t)S * S] = c2;
        }
    }

    std::vector<int64_t> shape{1, 3, S, S};
    std::vector<OnnxTensor> outs;
    if (!((OnnxModel*)model_)->run(input_.data(), shape, outs, err)) {
        dbgDetect("detect w=%d h=%d S=%d RUN-FAIL err=%s", w, h, S, err.c_str());
        return false;
    }

    if (wantDump) {
        dbgWrite("in.f32", input_.data(), input_.size() * sizeof(float));
        char nm[128];
        for (size_t i = 0; i < outs.size(); i++) {
            snprintf(nm, sizeof(nm), "out%02d_%s.f32", (int)i,
                     ((OnnxModel*)model_)->outputName((int)i));
            dbgWrite(nm, outs[i].data.data(), outs[i].data.size() * sizeof(float));
        }
        char info[1024];
        int n = snprintf(info, sizeof(info),
                         "w=%d h=%d S=%d scale=%.6f ox=%d oy=%d stride=%d flip=%d "
                         "preproc=%d provider=%s nout=%d\n",
                         w, h, S, (double)scale, ox, oy, stride, flipY ? 1 : 0,
                         preprocMode_, ((OnnxModel*)model_)->provider(), (int)outs.size());
        for (size_t i = 0; i < outs.size() && n > 0 && n < (int)sizeof(info); i++) {
            n += snprintf(info + n, sizeof(info) - (size_t)n, "out[%d]=%s shape=",
                          (int)i, ((OnnxModel*)model_)->outputName((int)i));
            for (size_t d = 0; d < outs[i].shape.size() && n < (int)sizeof(info); d++)
                n += snprintf(info + n, sizeof(info) - (size_t)n, "%s%lld",
                              d ? "," : "", (long long)outs[i].shape[d]);
            n += snprintf(info + n, sizeof(info) - (size_t)n, " n=%d\n",
                          (int)outs[i].data.size());
        }
        dbgWrite("dump.txt", info, strlen(info));
        dumpDone = 1;
    }

    // 12 个输出：cls/obj/bbox/kps × stride 8/16/32，按名字找（不依赖顺序）
    auto find = [&](const char* name) -> const OnnxTensor* {
        for (size_t i = 0; i < outs.size(); i++)
            if (strcmp(((OnnxModel*)model_)->outputName((int)i), name) == 0)
                return &outs[i];
        return nullptr;
    };

    std::vector<FaceBox> cand;
    for (int si = 0; si < 3; si++) {
        const int st = kStrides[si];
        char nb[16];
        snprintf(nb, sizeof(nb), "cls_%d", st);
        const OnnxTensor* cls = find(nb);
        snprintf(nb, sizeof(nb), "obj_%d", st);
        const OnnxTensor* obj = find(nb);
        snprintf(nb, sizeof(nb), "bbox_%d", st);
        const OnnxTensor* bbox = find(nb);
        if (!cls || !obj || !bbox) continue;
        snprintf(nb, sizeof(nb), "kps_%d", st);
        const OnnxTensor* kps = find(nb);   // 5 个点 × (x,y)，可缺省

        const int cols = S / st;
        const int rows = S / st;
        const int n = cols * rows;
        if ((int)cls->data.size() < n || (int)obj->data.size() < n) continue;
        if ((int)bbox->data.size() < n * 4) continue;

        for (int i = 0; i < n; i++) {
            const float score = sqrtf(cls->data[i] * obj->data[i]);
            if (score > bestScore_) bestScore_ = score;
            if (score < conf) continue;
            const int row = i / cols, col = i % cols;
            const float bx = bbox->data[i * 4 + 0];
            const float by = bbox->data[i * 4 + 1];
            const float bw = bbox->data[i * 4 + 2];
            const float bh = bbox->data[i * 4 + 3];
            const float cx = (col + bx) * st;
            const float cy = (row + by) * st;
            const float fw = expf(bw) * st;
            const float fh = expf(bh) * st;

            FaceBox f;
            // letterbox 坐标 → 原图坐标：先减黑边偏移，再除以缩放比
            const float vx = (cx - fw * 0.5f - ox) / scale;   // 视觉坐标（左上）
            const float vy = (cy - fh * 0.5f - oy) / scale;
            f.x = vx;
            f.w = fw / scale;
            f.h = fh / scale;
            // bottom-up 源：视觉坐标 → 缓冲行坐标（y' = h - y - 高度）
            f.y = flipY ? (h - vy - f.h) : vy;
            f.score = score;
            // 关键点解码与 bbox 同构：值相对 cell 原点，乘 stride 得 letterbox 坐标，
            // 再减黑边、除缩放比回到原图坐标。
            if (kps && (int)kps->data.size() >= (i + 1) * 10) {
                bool ok = true;
                for (int p = 0; p < 5; p++) {
                    const float kx = (col + kps->data[i * 10 + p * 2 + 0]) * st;
                    const float ky = (row + kps->data[i * 10 + p * 2 + 1]) * st;
                    f.kps[p * 2 + 0] = (kx - ox) / scale;
                    const float vy2 = (ky - oy) / scale;
                    f.kps[p * 2 + 1] = flipY ? (h - vy2) : vy2;
                    if (!(f.kps[p * 2 + 0] > -1e6f && f.kps[p * 2 + 0] < 1e6f)) ok = false;
                }
                f.hasKps = ok;
            }
            cand.push_back(f);
        }
    }

    // 贪心 NMS
    for (size_t i = 0; i < cand.size(); i++)
        for (size_t j = i + 1; j < cand.size(); j++)
            if (cand[j].score > cand[i].score) std::swap(cand[i], cand[j]);
    for (size_t i = 0; i < cand.size(); i++) {
        if (cand[i].score <= 0) continue;
        faces.push_back(cand[i]);
        for (size_t j = i + 1; j < cand.size(); j++)
            if (cand[j].score > 0 && iouOf(cand[i], cand[j]) > nms)
                cand[j].score = 0;
    }
    if (dbgDetectOn()) {
        float mn = 1e30f, mx = -1e30f; double sum = 0.0;
        for (size_t k = 0; k < input_.size(); k++) {
            const float v = input_[k];
            if (v < mn) mn = v; if (v > mx) mx = v; sum += v;
        }
        const double mean = input_.empty() ? 0.0 : sum / (double)input_.size();
        dbgDetect("detect w=%d h=%d S=%d nout=%d ncand=%d faces=%d best=%.4f flip=%d "
                  "in[min=%.1f max=%.1f mean=%.1f]",
                  w, h, S, (int)outs.size(), (int)cand.size(), (int)faces.size(), bestScore_,
                  flipY ? 1 : 0, mn, mx, mean);
    }
    lastMs_ = nowMs() - t0;
    return true;
}

}  // namespace beautycore
