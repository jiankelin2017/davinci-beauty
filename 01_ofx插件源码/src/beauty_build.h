#pragma once
// 构建版本串 —— 转出口。
//
// 唯一出处是 nama_engine.h 的 NAMA_BUILD_STR（四段式，当前 1.3.6.0）。这里只把
// 它转成一个函数，供不该为一串版本号就引入整份适配层头的编译单元使用：
//
//   beauty_core.cpp     含 beauty_core.h → beauty_params.h；
//   beauty_hwprobe.cpp  独立的 D3D 探测单元。
//
// 1.3.6.0 之前这里还承担「规避 C2365 重定义」的职责（那时 nama_engine.h 自己
// 另有一份 P_* 枚举）。枚举已合并到 beauty_params.h，这个理由不再成立；保留本
// 头是因为它仍然是最小依赖 —— 改版本号时不用把整个引擎接口拖进来。
//
// 在 beauty_core / beauty_hwprobe 里**不要**再写死版本号 —— 写死过一次
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
