# fccu-cpp

[![CI](https://github.com/DeguiLiu/fccu-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/fccu-cpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

C++17 header-only software **FCCU** (Fault Collection and Control Unit) for embedded systems.

**[中文文档](README_zh.md)** | Reference: [fccu_linux_demo](https://gitee.com/liudegui/fccu_linux_demo) (C version) | [newosp](https://github.com/DeguiLiu/newosp) (design patterns)

## Features

- **Header-only**: just `#include "fccu/fccu.hpp"`
- **Zero heap allocation**: all storage is stack/static
- **Bare-metal friendly**: no `std::thread`, no OS dependency
- **Priority queue set**: multi-level SPSC queues with admission control (60%/80%/99% thresholds)
- **Two-layer HSM**: global FCCU state machine (Idle/Active/Degraded/Shutdown) + per-fault lifecycle HSM
- **HookAction dispatch**: Handled / Escalate / Defer / Shutdown
- **Atomic bitmap**: fast active fault tracking with PopCount64
- **FaultReporter injection**: lightweight POD for zero-overhead wiring
- **Statistics**: per-priority atomic counters + recent fault ring
- **Optional integration**: mccc message bus notifications, ztask periodic scheduling

## Dependencies

| Library | Purpose | Required |
|---------|---------|----------|
| [ringbuffer](https://github.com/DeguiLiu/ringbuffer) | SPSC lock-free queue | Yes |
| [hsm-cpp](https://gitee.com/liudegui/hsm-cpp) | Hierarchical state machine | Yes |
| [mccc](https://github.com/DeguiLiu/mccc) | MPSC message bus | Optional (examples) |
| [ztask-cpp](https://github.com/DeguiLiu/ztask-cpp) | Cooperative task scheduler | Optional (examples) |
| [Catch2](https://github.com/catchorg/Catch2) v3 | Unit testing | Tests only |

All dependencies are fetched automatically via CMake FetchContent.

## Quick Start

```cpp
#include "fccu/fccu.hpp"

// Create collector: 16 max faults, 8-deep queues, 4 priority levels
fccu::FaultCollector<16, 8, 4> collector;

// Register fault points
collector.RegisterFault(0, 0x1001);  // Temperature sensor
collector.RegisterFault(1, 0x1002);  // Voltage monitor

// Register hook
collector.RegisterHook(0, [](const fccu::FaultEvent& e, void*) -> fccu::HookAction {
    printf("Fault 0x%04x occurred!\n", e.fault_code);
    return fccu::HookAction::kHandled;
});

// Report a fault (producer side)
collector.ReportFault(0, 0xDEAD, fccu::FaultPriority::kCritical);

// Process faults (consumer side, call from main loop or ztask)
collector.ProcessFaults();
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# Run tests
cd build && ctest --output-on-failure

# For China mainland, use mirror:
cmake -B build -DFCCU_GITHUB_MIRROR="https://ghfast.top/"
```

## Architecture

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

## Related Projects

| Project | Description |
|---------|-------------|
| [ringbuffer](https://github.com/DeguiLiu/ringbuffer) | C++14 SPSC lock-free ring buffer (queue infrastructure) |
| [hsm-cpp](https://gitee.com/liudegui/hsm-cpp) | C++14 hierarchical state machine (state management) |
| [mccc](https://github.com/DeguiLiu/mccc) | C++17 MPSC message bus (optional notification channel) |
| [ztask-cpp](https://github.com/DeguiLiu/ztask-cpp) | C++14 cooperative task scheduler (optional periodic driver) |
| [newosp](https://github.com/DeguiLiu/newosp) | C++17 embedded infrastructure library (design pattern source) |
| [fccu_linux_demo](https://gitee.com/liudegui/fccu_linux_demo) | C language FCCU reference implementation |

## License

MIT
