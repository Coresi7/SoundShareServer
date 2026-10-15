#pragma once

// ============================================================
// F6 (FR-07/FR-08) 进程级采集观测 —— S0 第一个落地项
//
// 依据：docs/tasks/T-GPU-AUDIO-GLITCH/02-design.md §3.0.1。
//   - header-only、无第三方依赖；位于既有 include 目录 ⇒ CMake 无需改
//     （CMakeLists.txt 显式列源，无新增 .cpp）。
//   - 单写者（采集线程）/ 多读者（探针），全部 relaxed 原子。
//   - m11：std::clamp / std::min / std::max 在本头文件内使用，必须自带
//     <algorithm>（main.cpp 先包含本头、后包含 <algorithm> 的包含顺序
//     不由本头控制）。
// ============================================================

#include <algorithm>   // ★ m11：本头内使用 std::min/std::max（编译级门禁 V-16）
#include <array>
#include <atomic>
#include <cstdint>

// 定宽直方图：250us 分辨率，4096 桶 → 饱和 1024ms；O(1) 写入，O(桶) 读分位。
// 分位口径（§3.0.1）：QuantileMs 走累计计数，返回桶下沿（保守上偏 ≤0.25ms）。
struct TimingHistogram {
    static constexpr uint32_t kBucketUs = 250;
    static constexpr uint32_t kBuckets  = 4096;
    std::array<std::atomic<uint32_t>, kBuckets> bins{};   // C++20 值初始化为 0
    std::atomic<uint64_t> count{0}, sumUs{0}, maxUs{0};
    std::atomic<uint64_t> saturated{0};   // ★ s2：落入饱和桶的次数（区分"真超 1.024s"与恢复期等待）

    void AddUs(uint64_t us) {
        const uint64_t bucket = us / kBucketUs;
        const uint32_t idx = (bucket >= kBuckets) ? (kBuckets - 1)
                                                  : static_cast<uint32_t>(bucket);
        if (bucket >= kBuckets) saturated.fetch_add(1, std::memory_order_relaxed);
        bins[idx].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        sumUs.fetch_add(us, std::memory_order_relaxed);
        // max 单调不减（CAS；统计用途）
        uint64_t prev = maxUs.load(std::memory_order_relaxed);
        while (us > prev &&
               !maxUs.compare_exchange_weak(prev, us,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {}
    }

    // 走累计计数，返回桶下沿（ms，两位小数由序列化层负责）。
    // 语义：返回值 v 满足"至少 q 比例的样本 ≤ v+250us 且 ≤ v 的不足 q"。
    double QuantileMs(double q) const {
        const uint64_t n = count.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        uint64_t need = 1;
        if (q > 0.0) {
            const double t = q * static_cast<double>(n);
            need = static_cast<uint64_t>(t);
            if (static_cast<double>(need) < t) need += 1;   // ceil
            if (need < 1) need = 1;
            if (need > n) need = n;
        }
        uint64_t acc = 0;
        for (uint32_t i = 0; i < kBuckets; ++i) {
            acc += bins[i].load(std::memory_order_relaxed);
            if (acc >= need) {
                return static_cast<double>(static_cast<uint64_t>(i) * kBucketUs) / 1000.0;
            }
        }
        return static_cast<double>(static_cast<uint64_t>(kBuckets - 1) * kBucketUs) / 1000.0;
    }

    void Reset() {
        for (uint32_t i = 0; i < kBuckets; ++i) {
            bins[i].store(0, std::memory_order_relaxed);
        }
        count.store(0, std::memory_order_relaxed);
        sumUs.store(0, std::memory_order_relaxed);
        maxUs.store(0, std::memory_order_relaxed);
        saturated.store(0, std::memory_order_relaxed);
    }
};

struct CaptureStats {
    // --- FR-07：机制 A 判据 D ---
    std::atomic<uint64_t> discontinuityTotal{0};   // DATA_DISCONTINUITY 累计
    std::atomic<uint64_t> silentTotal{0};          // SILENT 累计（对照）
    std::atomic<uint64_t> packets{0}, frames{0};
    // --- FR-08：机制 B 判据 P99 / F5 门控 C ---
    TimingHistogram interArrival;      // 采集循环「每次唤醒并取得数据」的间隔
    TimingHistogram callbackCritical;  // 采集回调关键路径（含持 L2 段）
    // ★ S-2（reviewer 建议）：到达间隔打点基准的跨窗口边界防护。
    //   ResetWindow() 递增本 epoch；采集线程打点时若发现 epoch 已变（说明
    //   stats_reset 落在两次唤醒之间），丢弃该次间隔（其 lastWakeUs 属于上一
    //   窗口，间隔不代表本窗口到达分布）。首例：baseline L2 窗 Max=66534.56ms
    //   即跨窗口边界首样本产物。
    std::atomic<uint64_t> arrivalEpoch{0};
    // --- F7 / F1 可观测 ---
    std::atomic<uint32_t> bufferHns{0}, bufferFrames{0};
    std::atomic<bool>     mmcssRegistered{false};
    std::atomic<bool>     eventDriven{false};
    std::atomic<uint64_t> eventHandlesCreated{0};  // ★ 硬门禁：重初始化不重建句柄 ⇒ 恒为 1
    std::atomic<uint64_t> eventTimeoutTotal{0};    // ★ m2：WaitForSingleObject 返回 WAIT_TIMEOUT 次数
    std::atomic<uint64_t> eventWakeTotal{0};       // 事件唤醒总次数（给 timeouts 提供分母）
    // --- FR-02（T-CAPTURE-IDLE-LOG §3.2.1）：空闲可观测（单写者=采集线程，relaxed
    //     原子，无新锁；沿上方 eventTimeoutTotal/eventWakeTotal 既有模式。S-6：C++
    //     成员不带 capture 前缀 = 沿用 eventTimeoutTotal 命名族；JSON 侧字段带
    //     capture 前缀，见 main.cpp）。---
    std::atomic<uint64_t> idleEpisodes{0};    // 进程级单调：进入 idle 时 +1（不随 ResetWindow 清零，P0-3）
    std::atomic<uint64_t> lastIdleMs{0};      // 快照：最近结束 episode 空闲时长 ms（随 ResetWindow 复位）
    std::atomic<uint64_t> totalIdleMs{0};     // 进程级单调：累计空闲 ms（含进行中 episode；不复位，P0-3）
    std::atomic<uint64_t> currentIdleMs{0};   // 快照：当前 episode 已持续 ms；非 idle 恒 0（随复位）
    std::atomic<uint64_t> reinitCount{0};
    std::atomic<bool>     endpointIsGpuAudio{false};
    // --- C3 标记注入 ---
    std::atomic<bool>     markerArmed{false};
    std::atomic<uint64_t> markerSeq{0}, markerT0QpcUs{0};
    std::atomic<bool>     reinitRequested{false};  // force_capture_reinit 镜像
    // capture_idle_sim 镜像（T-CAPTURE-IDLE-LOG §3.4.1 / D-02 / M-3）：FaultInjection
    // 是 HttpServer 成员、无单例 ⇒ 采集线程从本镜像原子读取（与 reinitRequested
    // 同模式）。diagnostics.cpp Set() 写入，采集线程 wasapi_capture.cpp 检查点读取。
    std::atomic<bool>     captureIdleSim{false};
    // --- F5（条件性）---
    std::atomic<uint64_t> workerQueueDrops{0};

    // 只清直方图与窗口基准；单调计数（D/silent/packets/frames/event*/reinit*）不清，
    // 窗口增量由采集脚本以两次探针读数差值取得（§6.2 / §7.7）。
    void ResetWindow() {
        interArrival.Reset();
        callbackCritical.Reset();
        // ★ S-2：递增 epoch —— 打点方据此丢弃跨 reset 边界的首个间隔样本
        arrivalEpoch.fetch_add(1, std::memory_order_relaxed);
        // --- FR-02（T-CAPTURE-IDLE-LOG §3.2.4 / P0-3 / AC-FR02-3）：reset 语义二分 ---
        lastIdleMs.store(0, std::memory_order_relaxed);     // 快照量 ⇒ 随窗口复位
        currentIdleMs.store(0, std::memory_order_relaxed);  // 快照量 ⇒ 随窗口复位
        // ★ P0-3：idleEpisodes / totalIdleMs 为进程级单调量 —— 此处【故意不复位】
        //   （真值源 = 采集线程局部 idleClosedMs，本函数够不到 ⇒ 单调性不受影响）。
        //   S-4 边界声明：totalIdleMs 的"进程级单调"以单次采集线程生命周期为界
        //   （Stop()/Start() 重建线程 ⇒ 局部 idleClosedMs 归零）；本任务全部 AC 在
        //   单次运行内判定，登记备查、非本任务回归。
    }
};

inline CaptureStats g_captureStats;
