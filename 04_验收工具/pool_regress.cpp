// pool_regress.cpp —— parallelRows 分块守恒回归（2026-11-08）
//
// 守护什么
// --------
// beauty_pool.cpp 的 run() 用 busy_ 计「还有几块没算完」，等它归零才返回。
// 历史缺陷：busy_ 记的是线程数 nt，而真实块数是 ceil(rows / chunk)，两者在
// 某些 (rows, nt) 组合下不相等（例：rows=65、nt=16 → chunk=5 → 只有 13 块）。
// 此时 busy_ 永远减不到 0，cvDone_ 不返回 —— 调用线程永久挂死，而且它此刻
// 还持有引擎的全局递归锁，整个插件（所有实例）一起卡住。
//
// 这个测试做两件事：
//   1) 把 (rows 1..320) × (nt 1..16) 全部跑一遍，逐行统计回调次数，要求
//      「每行恰好被执行一次」——覆盖漏行、重行、漏块三种错法；
//   2) 自带看门狗：20 秒没有进度就判定死锁并直接终止进程（退出码 2），
//      不让 CI 被一个 hang 拖住。
//
// 编译与运行见同目录 pool_regress.ps1；acceptance.py 的「并发」组会调用它。

#include "beauty_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

// 看门狗用的全局进度。主线程每跑完一个用例打一次，看门狗发现长时间不动就
// 认定死锁 —— 死锁的特征正是「进度停在某个用例上，线程池里没人再动」。
std::atomic<long long> g_progress{0};
std::atomic<int>      g_cases{0};
std::atomic<int>      g_lastRows{0};
std::atomic<int>      g_lastN{0};

void startWatchdog(unsigned timeoutMs) {
    std::thread([timeoutMs] {
        long long seen = -1;
        ULONGLONG lastMove = GetTickCount64();
        for (;;) {
            Sleep(200);
            const long long cur = g_progress.load(std::memory_order_relaxed);
            if (cur != seen) {
                seen = cur;
                lastMove = GetTickCount64();
                continue;
            }
            if (GetTickCount64() - lastMove > timeoutMs) {
                std::fprintf(stderr,
                             "\n[POOL] DEADLOCK rows=%d n=%d (cases done: %d)\n",
                             g_lastRows.load(), g_lastN.load(), g_cases.load());
                std::fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 2);
            }
        }
    }).detach();
}

// 跑一例：要求 [0, rows) 每一行都被回调命中且只命中一次。
bool oneCase(int rows, int n) {
    std::vector<std::atomic<int>> hit((size_t)(rows > 0 ? rows : 1));
    for (int i = 0; i < rows; i++) hit[(size_t)i].store(0);

    g_lastRows.store(rows);
    g_lastN.store(n);
    g_progress.fetch_add(1, std::memory_order_relaxed);

    beautycore::parallelRows(0, rows, n, [&](int b, int e) {
        // 越界或空块都算错：块必须落在 [0, rows) 内且 b < e。
        if (b < 0 || e > rows || b >= e) {
            std::fprintf(stderr, "[POOL] BAD BLOCK rows=%d n=%d [%d,%d)\n", rows, n, b, e);
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        for (int i = b; i < e; i++) hit[(size_t)i].fetch_add(1);
    });

    for (int i = 0; i < rows; i++) {
        if (hit[(size_t)i].load() != 1) {
            std::fprintf(stderr, "[POOL] FAIL rows=%d n=%d row %d hit %d times\n",
                         rows, n, i, hit[(size_t)i].load());
            std::fflush(stderr);
            return false;
        }
    }
    g_cases.fetch_add(1);
    g_progress.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace

int main() {
    std::printf("[POOL] parallelRows block-invariant regression\n");
    startWatchdog(20000);

    int fails = 0;

    // 1) 全量小尺寸扫描：历史缺陷的组合（如 65/16）全在这个区间里。
    for (int n = 1; n <= 16; n++)
        for (int rows = 1; rows <= 320; rows++)
            if (!oneCase(rows, n)) fails++;

    // 2) 真实帧尺寸（1080p 整帧 / 掩码带 / ROI 高度）—— 生产里最常见的口径。
    const int kReal[] = { 64, 65, 66, 71, 80, 96, 105, 120, 128, 240, 480, 540, 1080, 1920 };
    for (int n : { 2, 3, 4, 6, 8, 12, 16 })
        for (int rows : kReal)
            if (!oneCase(rows, n)) fails++;

    // 3) 线程数远超行数与行数极少：调用方钳制失效时的兜底（当前只靠池自保）。
    for (int n : { 7, 15, 16 }) {
        if (!oneCase(1, n)) fails++;
        if (!oneCase(2, n)) fails++;
        if (!oneCase(3, n)) fails++;
    }

    const int cases = g_cases.load();
    if (fails) {
        std::printf("[POOL] RESULT FAIL  cases %d, failed %d\n", cases, fails);
        return 1;
    }
    std::printf("[POOL] RESULT PASS  CASES %d OK  (each row exactly once, no hang)\n", cases);
    beautycore::poolShutdown();
    return 0;
}
