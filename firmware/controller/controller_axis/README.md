# controller_axis/ — 轴模块

```text
controller_axis/
├── include/controller_axis/
│   ├── axis.h             单轴对象、请求与回执接口
│   └── controller_axis.h  整机轴集合接口
├── axis/
│   └── axis.c             单轴请求转发、下行单位换算与回执消费
├── controller_axis.c      多轴请求与结果汇总
└── README.md
```

## 职责

- 每个轴对应一个 CiA 402 设备实例和独立的对象访问通道。
- 单轴提交配置、使能、运动、暂停和急停请求，提供 `axis_take_motion()` 回执；
  失败时一并带回设备层归类（超时 / 驱动器故障 / abort / 回读不符）与 abort 码。
- 单轴完成下行命令的 `mm → pulse` 换算、取整和饱和处理，参数位于 `config/cats_mech.h`。
- 整机轴集合逐轴下发请求，保存调用方的完成位图并汇总等待、成功或失败。
- 配置、使能、急停三个逐轴动作共用同一套轴号解析与遍历，`CONTROLLER_AXIS_ALL` 由头文件公开。
- 整机轴集合用调用方给的同一时刻推进每个轴的设备状态机，时钟归组合根持有。
- 反馈读取和上行单位换算由控制器负责，轴模块不调度反馈读取。

## 边界

```text
states → controller_axis → axis → cia402 → sdo_access
```

轴模块不解释控制字、状态字或对象号，不直接访问 CAN，也不创建线程。整机轴集合只
接收轴数组、轴数和完成位图，不依赖控制器私有上下文。
