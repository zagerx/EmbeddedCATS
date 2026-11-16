# controller_axis/ — 轴模块

```text
controller_axis/
├── include/controller_axis/
│   ├── axis.h             单轴对象、请求与反馈接口
│   └── controller_axis.h  整机轴集合接口
├── axis/
│   └── axis.c             单轴请求转发、单位换算与回执消费
├── controller_axis.c      多轴请求与结果汇总
└── README.md
```

## 职责

- 每个轴对应一个 CiA 402 设备实例和独立的对象访问通道。
- 单轴提交配置、使能、运动、暂停和急停请求，提供 `axis_take_motion()` 回执。
- 单轴完成 `mm ↔ pulse` 换算、取整和饱和处理，换算参数位于 `config/cats_mech.h`。
- 整机轴集合逐轴下发请求，保存调用方的完成位图并汇总等待、成功或失败。
- 控制器读取原始反馈，单轴只负责换算；轴模块不调度反馈读取。

## 边界

```text
states → controller_axis → axis → cia402 → sdo_access
```

轴模块不解释控制字、状态字或对象号，不直接访问 CAN，也不创建线程。整机轴集合只
接收轴数组、轴数和完成位图，不依赖控制器私有上下文。
