# lyos_node/ — 集成层（orin-agx 形态，占位）

将来接入 orin-agx 平台时，本目录放一个 lyos 进程：

```text
main → lyos::init("cats_driver") → 解析 --etc → 读 yaml
     → 选择平台 backend → 构造 app → 周期运行 → lyos::spin()
```

- 语言：**C++**
- 对外只做 topic 映射（订阅控制指令、发布反馈与健康状态）
- 内核层、设备层、轴层**不改一行**；只需完成本目录 + `backends/orin_agx/`

平台框架、进程模型与 topic 约定见 [`../../../docs/01_设计文档/orin-agx平台框架说明.md`](../../../docs/01_设计文档/orin-agx平台框架说明.md)。

> 当前为空目录骨架，接口待与平台侧对齐后填写。
