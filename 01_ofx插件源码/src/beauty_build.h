#pragma once
// 构建版本串 —— 转出口。
//
// 唯一出处是 nama_engine.h 的 NAMA_BUILD_STR（四段式，如 1.3.5.0）。这里只把
// 它转成一个函数，供**不能直接包含 nama_engine.h** 的编译单元使用：
//
//   beauty_core.cpp     含 beauty_core.h → beauty_params.h，那份 P_* 枚举与
//                       nama_engine.h 里的同名（下标逐位一致，名字略异：
//                       P_THIN_FACE/P_BIG_EYE vs P_CHEEK/P_EYE），同时包含会报
//                       C2365「重定义」；
//   beauty_hwprobe.cpp  独立的 D3D 探测单元，没有理由为了一个宏引入整份适配层头。
//
// 所以在 beauty_core / beauty_hwprobe 里**不要**再写死版本号 —— 写死过一次
// （"1.4.0"），结果日志里出现 build=1.3.5.0 与 ver=1.4.0 并存，被当成插件版本
// 排查了半天。改名或改值时只改 nama_engine.h。

#ifdef __cplusplus
extern "C" {
#endif

// 返回指向静态存储的版本串（生命周期 = 整个进程），调用方不要释放。
const char* namaBuildStr(void);

#ifdef __cplusplus
}
#endif
