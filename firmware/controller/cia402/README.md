# cia402/ — CiA 402 设备模块

统一包含 DS402 标准知识和单个从站的运行控制，但内部保持两条边界：

- `pds.c`：纯函数，不访问总线、不读取时钟、不创建线程。
- `device*.c`：请求、超时、取消、同步流程和反馈，只依赖 `transport/sdo_access.h`。

一个 `struct cia402_device` 对应一个从站。双轴驱动器呈现为两个 CANopen 节点，因此创建两个实例。

## 结构

```text
cia402/
├── include/cia402/
│   ├── types.h    状态、控制字/状态字位、模式号
│   ├── od.h       DS402 对象号
│   ├── pds.h      状态解析、序列表、起点快进
│   └── device.h   设备请求、结果与反馈
└── src/
    ├── pds.c
    ├── device.c       请求槽、服务执行、反馈轮转
    ├── device_seq.c   控制字序列、故障复位、Halt、Quick Stop
    ├── device_pp.c    PP 参数、触发与到位判断
    └── device_internal.h
```

## 纯 PDS 知识

- `cia402_parse(sw)`：解析 6041 状态字。
- `cia402_seq_enable()` / `cia402_seq_disable()`：提供控制字与期望状态字序列。
- `cia402_seq_skip()`：根据当前状态跳过已经完成的步骤。

使能序列为 `0x06 → 0x07 → 0x0F`，期望 PDS 状态依次为 Ready、Switched on、Operation enabled。

## 设备运行模型

控制线程只提交请求，服务线程同步执行完整流程：

```text
cia402_device_request_* / cia402_device_pp_start
                ↓
cia402_device_service（服务线程）
                ↓
sdo_access.read/write（阻塞等待）
```

普通请求一次只执行一个；Halt 和 Quick Stop 会取消在途流程并优先占用请求槽。反馈轮转只在设备空闲时读取，避免与业务流程争用同一通道。

## 依赖边界

```text
cia402/device → sdo_access ← transport/sdo_client → CANopenNode
cia402/pds    → 无依赖
```

本模块不认识 CAN 帧、COB-ID、毫米单位或整机业务状态机。`sdo_client` 不属于 CiA 402 核心，因为同一设备层也可以使用其他对象字典传输实现。

## 自测

`apps/tools/cia402_selftest` 覆盖状态字解析与使能起点快进，不需要 CAN 硬件。
