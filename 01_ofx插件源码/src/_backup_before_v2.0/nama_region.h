#pragma once
// NamaRegion —— fuai.dll 语义区域提供者（皮肤分割）。
//
// 为什么需要它
// ------------
// v1.4 的「只磨皮肤区域」和「美白」都靠逐像素颜色判据（Cr/Cb + R>G>B）。实测
// 白色蕾丝上衣、木桌、米色墙都落在这个色域里 —— 面板美白 100 时非皮肤区有
// 40%~77.5% 的像素被提亮，白衣服直接被推到接近 255、纹理全丢。
// 详见 02_测试夹具beauty_probe\PHASE1_区域精准化探测报告.md。
//
// SDK 侧没有任何可用的区域接口：aitype 是精确枚举不是位掩码（OR 任何位都导致
// 人脸模型不加载），皮肤分割模型虽然一直在跑，但美颜道具不消费它的结果，
// 唯一的开关在 bundle 的 JS 控制器里，我们的集成路径走不到那一层。
//
// 所以这里直接驱动 fuai.dll 拿模型级皮肤分割：喂一张 RGBA，吐一张
// 192×256 的 float 概率图（整图各向异性拉伸，不是人脸局部裁剪）。
//
// 实测成本（1080×1920，检出 1 张脸）
//   - 初始化（读 23MB bundle + tflite）：约 600 ms，放在 createInstance 预热
//   - 单次推理（检测 + 关键点 + 皮肤分割）：约 15.6 ms
//   - 耗时与分辨率基本无关（模型输入尺寸固定），所以只能靠**降频复用**：
//     每 N 帧推理一次、其余帧沿用上次掩码，N=3 时摊到约 5.2 ms/帧
//
// 稳定性
// ------
// fuai 是逆向对接的闭源 DLL，跑在宿主进程里。所有调用都包 SEH；一旦抛访问
// 违例就把自己标成 failed_，本进程内不再调用，调用方退回 v1.4 的颜色启发式。

#include <string>
#include <vector>

class NamaRegion {
public:
    static NamaRegion& inst();

    // engineDir 里必须有 fuai.dll 与明文 bundle（见 findBundle）。
    // 失败时返回 false 并写 err；失败会被记住，不会每帧重试。
    bool ensureReady(const std::string& engineDir, std::string& err);
    bool ready() const { return ready_; }
    bool failed() const { return failed_; }
    const char* lastErr() const { return err_.c_str(); }

    // 逐帧入口：every 帧真正推理一次，其余帧复用上次掩码。
    // rgba 是 RGBA8，w*h*4 字节。返回掩码（maskW×maskH，0..255），
    // nullptr = 本次没有可用掩码（未就绪 / 失败 / 连续推理都没检出人脸）。
    const unsigned char* update(const unsigned char* rgba, int w, int h, int every);

    int maskW() const { return mw_; }
    int maskH() const { return mh_; }
    // 掩码里 >=128 的像素占全图的比例（0..1）。用来判断「这次分割是不是靠谱」。
    double coverage() const { return coverage_; }
    int lastFaces() const { return faces_; }
    double lastInferMs() const { return inferMs_; }
    long inferCount() const { return inferCount_; }

    void release();

private:
    NamaRegion() {}
    ~NamaRegion() {}
    NamaRegion(const NamaRegion&) = delete;
    NamaRegion& operator=(const NamaRegion&) = delete;

    bool loadDll(std::string& err);
    bool loadBundle(std::string& err);
    // 真正跑一次推理并刷新 mask_。调用前必须已 ready_。
    bool inferOnce(const unsigned char* rgba, int w, int h);
    std::string findBundle() const;

    bool ready_ = false;
    bool failed_ = false;
    std::string err_;
    std::string dir_;

    void* hDll_ = nullptr;
    void* proc_ = nullptr;     // FUAI face processor
    void* result_ = nullptr;   // FUAI result
    void* view_ = nullptr;     // FUAI image view
    std::vector<unsigned char> bundle_;

    int mw_ = 0, mh_ = 0;
    std::vector<unsigned char> mask_;   // mw_*mh_，0..255
    bool hasMask_ = false;
    long frameSeq_ = 0;
    int faces_ = 0;
    double coverage_ = 0.0;
    double inferMs_ = 0.0;
    long inferCount_ = 0;
};
