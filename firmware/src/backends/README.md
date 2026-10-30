# backends/ — 边界层

`canopen/include/canopen/can_io.h` 的实现。**移植时唯一需要替换的地方。**

| 子目录 | 说明 | 状态 |
|---|---|---|
| `socketcan/` | 本机 Ubuntu 实现，**唯一允许出现 `linux/can.h` 的目录** | 待实现 |
| `loopback/` | 假后端：无硬件时跑单测与回放 | 待实现 |
| `orin_agx/` | 平台侧实现占位，由负责 orin-agx 驱动的人填充 | 占位 |

## 约束

- 只实现 `can_io.h` 声明的能力，不向上暴露任何平台细节；
- 不 include `cia402/`、`axis/`、`app/` 的任何头文件；
- 错误一律转换为 `can_io.h` 中定义的错误码后上报。

> 当前为空目录骨架。
