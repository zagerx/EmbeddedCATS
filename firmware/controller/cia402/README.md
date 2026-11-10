# cia402/ — 实例化 CiA 402 设备状态机

一个 `struct cia402_device` 对应一个从站。每个实例独立保存请求、状态机、传输进度、超时和反馈，不使用跨设备共享的可写状态。

## 结构

```text
cia402/
├── include/cia402/
│   ├── types.h    PDS 状态、控制字/状态字位、模式号
│   ├── od.h       DS402 对象号
│   ├── pds.h      状态解析与序列表
│   └── device.h   设备实例、请求、结果与反馈
└── src/
    ├── pds.c            纯标准知识
    ├── device.c         请求与结果、事件调度、取消、反馈
    ├── device_seq.c     配置、使能、断使能、Halt、Quick Stop
    ├── device_pp.c      PP 参数、触发和到位状态
    └── device_internal.h
```

## 事件状态机

服务线程调用 `cia402_device_service()`，每次只处理一个事件：

```text
提交请求 / 收到 SDO 结果 / 事务超时 / 取消完成
                    ↓
            计算下一状态并发起下一笔事务
                    ↓
                  返回
```

配置、使能、PP 和停止均不阻塞。状态机通过异步 `sdo_access` 发起对象读写，不使用循环等待或条件变量。

使能按实际 6041 状态决定路径：

```text
Switch on disabled → 0x06 → Ready
Ready              → 0x07 → Switched on
Switched on        → 0x0F → Operation enabled
Fault              → 故障复位状态链
```

PP 依次写速度、加速度、减速度和目标位置，以 6040 bit4 边沿触发，再通过 6041 bit10 和必要的 6064 读数判断到位。

Halt 与 Quick Stop 会抢占当前流程。服务线程先取消在途事务并丢弃旧结果，再启动停止状态链；Quick Stop 即使前序访问失败也继续尝试断使能。

单次 SDO 超时与整个使能步骤、停车、PP 到位的业务期限分别管理。反馈只在设备空闲时轮转读取。

## 依赖边界

```text
cia402/device → sdo_access ← transport/sdo_client → CANopenNode
cia402/pds    → 无依赖
```

本模块不认识 CAN 帧、COB-ID、毫米单位或整机业务状态机。

## 自测

`apps/tools/cia402_selftest` 不需要 CAN 硬件，覆盖：

- PDS 状态字解析与序列起点快进；
- 完整使能状态链；
- PP 参数、触发和到位；
- Quick Stop 抢占在途事务；
- 事务超时与失败发布。
