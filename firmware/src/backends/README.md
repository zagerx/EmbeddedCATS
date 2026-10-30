# backends/ — 边界层

`canopen/include/canopen/can_io.h` 的实现。**移植时唯一需要替换的地方。**

| 子目录 | 说明 | 状态 |
|---|---|---|
| `socketcan/` | 本机 Ubuntu 实现，**唯一允许出现 `linux/can.h` 的目录** | 已实现 `send/recv/close/now_ms` |
| `loopback/` | 假后端：无硬件时跑单测与回放 | 待实现 |
| `orin_agx/` | 平台侧实现占位，由负责 orin-agx 驱动的人填充 | 占位 |

`socketcan` 的公共头是 `include/socketcan/can_io_socketcan.h`，只暴露一个工厂函数：

```c
static socketcan_dev_t dev;                 /* 存储由调用方给，全程不用 malloc */
can_io_t io;
socketcan_open(&io, &dev, "can0");          /* 装配：填好 io.ops / io.ctx */
```

装配由集成层做，内核不调用它。遍历帧的 COB-ID 过滤放内核（后端不过滤）。

## 约束

- 只实现 `can_io.h` 声明的能力，不向上暴露任何平台细节；
- 不 include `cia402/`、`axis/`、`app/` 的任何头文件；
- 错误一律转换为 `can_io.h` 中定义的错误码后上报。

> 当前为空目录骨架。
