// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Jian
#pragma once
// ---------------------------------------------------------------------------
// beauty_params.h —— 面板参数（**唯一出处**）
//
// 本文件只放「插件层与引擎层都要知道」的纯数据：
//   * 面板参数枚举（P_*）
//   * 面板设置结构体（BeautySettings）
//
// 版本号**不在这里**。唯一出处是 beauty_engine.h 的 BEAUTY_BUILD_*（四段式），
// 需要字符串时调 beauty_build.h 的 beautyBuildStr()，别在本文件再写一份字面量
// 副本 —— 两份版本号必然走散。
//
// 为什么单独拆出来：本文件原先长在引擎头里，导致插件为了拿一个十余项的
// double 数组，被迫连整个引擎适配层一起 include。现在插件侧只依赖这份纯数据头。
// ---------------------------------------------------------------------------

#include <cstddef>

// ---------------------------------------------------------------------------
// 面板参数顺序 —— 滤镜属性页滑块的顺序
//
// ⚠️ 本枚举是**全仓库唯一一份**，`ofx_plugin.cpp` 的 kParams 必须与它逐项对应：
// readParams() 按下标写 panel[]，顺序错一位就是「拖 A 滑块、B 效果变」，
// 而编译期只能保证个数相同。新增面板项只改这里，并同步改 kParams。
// ---------------------------------------------------------------------------
enum {
    P_BLUR = 0,      // blur_level              磨皮   max_val 6.0
    P_CLARITY,       // clarity                 清晰   max_val 1.0
    P_WHITEN,        // color_level             美白   max_val 2.0
    P_RED,           // red_level               红润   max_val 1.0
    P_SHARPEN,       // sharpen                 锐化   max_val 1.0
                     //   底层接线保留：磨皮=0 时它确实有效，旧工程的非零值不能丢。
    P_GLOBAL_SHARP,  // （本地新增项，无外部对应） 全局锐化 max_val 1.0
                     //   把与「锐化」同一条曲线的细节回填扩展到皮肤掩码**之外**。
                     //   0 = 脸外逐字节不变（默认，保持「只动脸」这条硬指标）。
    P_THREED,        // face_threed             五官立体 max_val 1.0
    P_EYEBRIGHT,     // eye_bright              亮眼   max_val 1.0
    P_POUCH,         // remove_pouch_strength   去黑眼圈 max_val 1.0
    P_NASOLABIAL,    // remove_nasolabial_folds_strength 去法令纹 max_val 1.0
    P_DELSPOT,       // delspot_level           祛斑痘 max_val 1.0
                     //   脸内效果量随档位上升，但帧间闪动同步上升（实测约 +56%），
                     //   所以默认 0，由用户按机器性能与观感自行决定开多大。

    // ---- 美形三项（几何变形，与上面十项「颜色/贴图类」不同）----
    // 上面十项是逐像素颜色变换或局部椭圆增益，像素位置从不改变；这三项改的是
    // **像素位置**（反向映射 + 双线性重采样），所以在管线末端单独跑一趟，
    // 顺序不能混进前面（见 beauty_core.cpp 末尾）。
    //
    // 只挑三个正交维度：脸宽 / 眼睛大小 / 下庭长度。刻意不接同一族里的
    // cheek_narrow / cheek_v / cheek_small / cheek_short —— 它们与 thin_face
    // 抢同一块像素，同时推会互相打架。
    //
    // ⚠️ 这三项在面板里**沉底**（`ofx_plugin.cpp` 的 kParams 同样换序，两者必须
    // 逐一对应）。原因：Resolve 的 OFX 面板不支持任何折叠分组（Page / 页内 Group /
    // root Group 三种均实测无效），折叠走不通，退而求其次把低频项排到列表最末，
    // 常用项保持在顶部。
    P_THIN_FACE,     // thin_face               瘦脸   max_val 1.0
    P_BIG_EYE,       // big_eye                 大眼   max_val 1.0
    P_CHIN,          // chin                    下巴   max_val 1.0
    P_COUNT
};

struct BeautySettings {
    double panel[P_COUNT];   // 面板刻度 0..100
    int blur_type;           // 0=双边滤波(默认,保边) 1=高斯(不保边) 2=弱磨皮(强度对折)
    int skin_detect;         // 0/1 只磨皮肤区域（默认 1：不要把背景一起磨糊）
    int smooth_mode;         // 0 人像磨皮（默认，逐位等于上一版行为）
                             // 1 清晰磨皮 = 平坦皮肤少回填、边缘轮廓多回填（推荐）
                             // 2 精细磨皮 = 整体收敛，只留关键轮廓
                             // 3 朦胧磨皮 = 整体压低细频

    BeautySettings() : blur_type(0), skin_detect(1), smooth_mode(0) {
        // 默认值：只开「磨皮 20」，其余十三项 0，几何变形三项默认关（改人脸轮廓
        // 属于「用户主动要才做」的一类，默认关掉保证升级后画面逐字节不变）。
        // 强默认值会让用户第一眼就觉得「假」，所以磨皮只给 20 而不是拉满。
        const double d[P_COUNT] = {20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0};
        for (int i = 0; i < P_COUNT; i++) panel[i] = d[i];
    }

    // 十四项全 0 —— 引擎输出与输入逐字节一致，可以直接透传，不必进引擎。
    bool allZero() const {
        for (int i = 0; i < P_COUNT; i++)
            if (panel[i] != 0.0) return false;
        return true;
    }
};
