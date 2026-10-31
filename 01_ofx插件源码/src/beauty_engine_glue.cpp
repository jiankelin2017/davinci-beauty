// beauty_engine_glue.cpp —— core_glue 桥接层的实现（唯一接触 CoreEngine 的翻译单元）
//
// 职责：把 NamaEngine 侧传来的原始参数转成 BeautySettings，再转调 OBS 自研
// CoreEngine（纯 CPU 引擎，人脸检测走 ONNX，内部自动检测 GPU/DirectML）。
// 本文件 include 了 beauty_core.h（含 beauty_params.h 的 P_* 枚举），但 nama_engine.cpp
// 只通过 core_glue.h 调用本文件，因此不会把两份 P_* 枚举拉进同一个编译单元。

#include "core_glue.h"
#include "beauty_core.h"     // IBeautyEngine, beautyEngine(), BeautySettings
#include "beauty_onnx.h"     // beautycore::onnxSetRuntimeDir
#include <cstring>

namespace bc = beautycore;

static IBeautyEngine& eng() { return beautyEngine(); }

static void putErr(char* errBuf, int errCap, const std::string& e) {
    if (errBuf && errCap > 0) {
        strncpy(errBuf, e.c_str(), (size_t)errCap - 1);
        errBuf[errCap - 1] = '\0';
    }
}

bool coreBeautyEnsureReady(const char* ortDir, char* errBuf, int errCap) {
    if (ortDir && *ortDir) bc::onnxSetRuntimeDir(ortDir);
    std::string e;
    bool ok = eng().ensureReady(e);
    if (!ok) putErr(errBuf, errCap, e);
    return ok;
}

bool coreBeautyPreheat(const char* ortDir, char* errBuf, int errCap, int* didInit) {
    if (ortDir && *ortDir) bc::onnxSetRuntimeDir(ortDir);
    std::string e;
    bool di = false;
    bool ok = eng().preheat(e, di);
    if (didInit) *didInit = di ? 1 : 0;
    if (!ok) putErr(errBuf, errCap, e);
    return ok;
}

bool coreBeautyReady() { return eng().ready(); }

int coreBeautyAcquireSlot(char* errBuf, int errCap) {
    std::string e;
    int s = eng().acquireSlot(e);
    if (s < 0) putErr(errBuf, errCap, e);
    return s;
}

void coreBeautyReleaseSlot(int slot) { eng().releaseSlot(slot); }

void coreBeautySetDetectOptions(int r, int q, int m) { eng().setDetectOptions(r, q, m); }

void coreBeautySetImageBottomUp(int bottomUp) { eng().setImageBottomUp(bottomUp != 0); }

void coreBeautySetAsyncDetect(int on) { eng().setAsyncDetect(on != 0); }

bool coreBeautyProcess(int slot, const unsigned char* src, unsigned char* dst,
                       int w, int h, const double panel[14],
                       int blur_type, int skin_detect, int smooth_mode,
                       bool advanceFrame) {
    BeautySettings bs;
    for (int i = 0; i < 14; i++) bs.panel[i] = panel[i];
    bs.blur_type    = blur_type;
    bs.skin_detect  = skin_detect;
    bs.smooth_mode  = smooth_mode;
    return eng().process(slot, src, dst, w, h, bs, advanceFrame);
}

int         coreBeautyLastFaces()     { return eng().lastFaces(); }
double      coreBeautyLastFaceScore() { return eng().lastFaceScore(); }
int         coreBeautyItemId()        { return eng().itemId(); }
const char* coreBeautyVersion()       { return eng().version(); }
const char* coreBeautyLastErr()       { const char* p = eng().lastErr(); return p ? p : ""; }
double      coreBeautyLastCoreMs()    { return eng().lastCoreMs(); }
bool        coreBeautyRegionActive()  { return eng().regionActive(); }
const char* coreBeautyGpuProvider()  { return eng().gpuProvider(); }
void        coreBeautyRelease()       { eng().release(); }
