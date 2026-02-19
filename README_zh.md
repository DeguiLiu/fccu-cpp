# fccu-cpp

[![CI](https://github.com/DeguiLiu/fccu-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/fccu-cpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

C++17 header-only 软件 **FCCU** (Fault Collection and Control Unit, 故障收集与控制单元)，面向嵌入式系统。

> C 语言参考实现: [fccu_linux_demo](https://gitee.com/liudegui/fccu_linux_demo)
> 设计模式参考: [newosp](https://github.com/DeguiLiu/newosp) fault_collector.hpp

## 什么是 FCCU

FCCU 是汽车/工业 MCU 中常见的硬件模块，负责收集系统各模块的故障信号，按优先级分类处理，并触发相应的恢复或关停动作。fccu-cpp 是该概念的纯软件实现，将硬件 FCCU 的核心机制抽象为 C++ 模板库，适用于:

- 工业嵌入式设备 (激光雷达、机器人控制器、边缘网关)
- 汽车电子 ECU 软件层故障管理
- 需要集中化故障收集的裸机/RTOS 系统

## 核心功能

### 故障收集与分发

- **故障注册**: 每个故障点绑定故障码、属性、错误阈值
- **优先级上报**: 4 级优先级 (Critical/High/Medium/Low)，高优先级故障优先处理
- **用户 Hook**: 每个故障可注册回调，返回处理动作:
  - `Handled` -- 故障已处理，清除活跃位
  - `Escalate` -- 升级到更高优先级重新入队
  - `Defer` -- 保持活跃，稍后再处理
  - `Shutdown` -- 请求系统关停
- **故障升级**: Hook 返回 Escalate 时，自动以更高优先级重新入队

### 多级优先级队列

- 基于 [ringbuffer](https://github.com/DeguiLiu/ringbuffer) SPSC 无锁环形缓冲
- 每个优先级独立队列，高优先级队列优先出队
- **准入控制** (来自 newosp 模式):
  - Critical: 始终准入
  - High: 队列 < 99% 时准入
  - Medium: 队列 < 80% 时准入
  - Low: 队列 < 60% 时准入

### 两层层次状态机 (HSM)

基于 [hsm-cpp](https://gitee.com/liudegui/hsm-cpp) 实现:

**全局 FCCU 状态机**:

```
Idle --FaultReported--> Active --CriticalDetected--> Degraded
Active --AllCleared--> Idle
Active/Degraded --ShutdownReq--> Shutdown
Degraded --DegradeRecovered--> Active
```

**Per-Fault 状态机** (可选，最多 8 个关键故障):

```
Dormant --> Detected --> Active --> Recovering --> Cleared --> Dormant
```

### 故障状态追踪

- **原子位图**: `fetch_or` / `fetch_and` + `PopCount64` 快速统计活跃故障数
- **统计计数器**: per-priority 原子计数 (reported/processed/dropped)
- **近期故障环**: 16 槽环形缓冲，支持从最新到最旧遍历
- **背压监控**: Normal/Warning/Critical/Full 四级背压等级

### 集成接口

- **FaultReporter 注入点**: 16 字节 POD 结构，零开销故障上报绑定
- **mccc 总线通知**: 故障处理时通过 [mccc](https://github.com/DeguiLiu/mccc) 消息总线发布通知 (可选)
- **ztask 周期调度**: 通过 [ztask-cpp](https://github.com/DeguiLiu/ztask-cpp) 协作式调度器周期调用 ProcessFaults (可选)

## 设计特性

| 特性 | 实现方式 |
|------|---------|
| Header-only | 仅 `#include "fccu/fccu.hpp"` |
| 零堆分配 | 所有存储栈/静态分配 |
| 裸机友好 | 无 `std::thread`，无 OS 依赖 |
| 编译期配置 | 模板参数: MaxFaults, QueueDepth, QueueLevels, MaxPerFaultHsm |
| SPSC 线程模型 | 单生产者上报，单消费者处理 (裸机/协作式调度) |

## 与 newosp FaultCollector 的对比

| 维度 | newosp FaultCollector | fccu-cpp |
|------|----------------------|----------|
| 队列 | 内置 MPSC CAS 队列 | 外部 ringbuffer (SPSC) |
| 消费者 | std::thread + condition_variable | 外部调用 ProcessFaults() (无线程) |
| 状态管理 | atomic bool | hsm-cpp 两层 HSM |
| 通知 | 无 | mccc AsyncBus (可选) |
| 平台 | Linux (std::thread) | 裸机友好 (无 OS 依赖) |

## 依赖

| 库 | 用途 | 是否必需 |
|----|------|---------|
| [ringbuffer](https://github.com/DeguiLiu/ringbuffer) | SPSC 无锁队列 | 必需 |
| [hsm-cpp](https://gitee.com/liudegui/hsm-cpp) | 层次状态机 | 必需 |
| [mccc](https://github.com/DeguiLiu/mccc) | MPSC 消息总线 | 可选 (示例) |
| [ztask-cpp](https://github.com/DeguiLiu/ztask-cpp) | 协作式任务调度 | 可选 (示例) |
| [Catch2](https://github.com/catchorg/Catch2) v3 | 单元测试 | 仅测试 |

所有依赖通过 CMake FetchContent 自动拉取。

## 快速开始

```cpp
#include "fccu/fccu.hpp"

// 创建收集器: 16 个最大故障点, 8 深队列, 4 个优先级
fccu::FaultCollector<16, 8, 4> collector;

// 注册故障点
collector.RegisterFault(0, 0x1001);  // 温度传感器
collector.RegisterFault(1, 0x1002);  // 电压监控

// 注册 Hook
collector.RegisterHook(0, [](const fccu::FaultEvent& e, void*) -> fccu::HookAction {
    printf("Fault 0x%04x!\n", e.fault_code);
    return fccu::HookAction::kHandled;
});

// 上报故障 (生产者侧)
collector.ReportFault(0, 0xDEAD, fccu::FaultPriority::kCritical);

// 处理故障 (消费者侧, 在主循环或 ztask 回调中调用)
collector.ProcessFaults();
```

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# 运行测试
cd build && ctest --output-on-failure

# 中国大陆使用镜像加速:
cmake -B build -DFCCU_GITHUB_MIRROR="https://ghfast.top/"
```

## 架构

```
                    +---------------------------------------+
                    |       FaultCollector<Config>           |
                    |                                       |
ReportFault() --->  |  FaultTable    GlobalHsm              |
                    |  (array)       Idle/Active/            |
                    |                Degraded/Shutdown       |
                    |                                       |
                    |  FaultQueueSet                         |
                    |  spsc::Ringbuffer per level            |
                    |  + priority admission control          |
                    |                                       |
ProcessFaults() --> |  HookAction dispatch                   |
                    |  Handled/Escalate/Defer/Shutdown       |
                    |                                       |
                    |  Per-Fault HSM (optional, <=8)         |
                    |  Dormant->Detected->Active->Cleared    |
                    |                                       |
                    |  Atomic bitmap + Stats + Recent ring   |
                    +-------+---------------+---------------+
                            |               |
                     mccc AsyncBus    ztask Scheduler
                     (optional)       (optional)
```

## 相关项目

| 项目 | 说明 |
|------|------|
| [ringbuffer](https://github.com/DeguiLiu/ringbuffer) | C++14 SPSC 无锁环形缓冲，fccu-cpp 的队列基础设施 |
| [hsm-cpp](https://gitee.com/liudegui/hsm-cpp) | C++14 层次状态机，fccu-cpp 的状态管理基础设施 |
| [mccc](https://github.com/DeguiLiu/mccc) | C++17 MPSC 消息总线，可选的故障通知通道 |
| [ztask-cpp](https://github.com/DeguiLiu/ztask-cpp) | C++14 协作式任务调度器，可选的周期处理驱动 |
| [newosp](https://github.com/DeguiLiu/newosp) | C++17 嵌入式基础设施库，fccu-cpp 的设计模式来源 |
| [fccu_linux_demo](https://gitee.com/liudegui/fccu_linux_demo) | C 语言 FCCU 参考实现 |

## License

MIT
