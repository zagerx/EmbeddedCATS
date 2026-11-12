# bus/ — 一帧 CAN 的收发能力（边界层）

**整个工程唯一允许出现 `linux/can.h`、socket、`poll` 的地方。**

只定义一件事：一帧怎么发出去、怎么收进来。不认识任何协议、COB-ID 语义、线程、
业务词汇。

## 结构

```text
bus/
├── include/bus/can_io.h      收发契约（上层只 include 这里）
├── socketcan/                本机 Ubuntu 实现
│   ├── include/socketcan/can_io_socketcan.h
│   └── src/can_io_socketcan.c
├── loopback/                 假后端（无硬件单测 / 回放），待实现
└── orin_agx/                 平台侧实现占位，由平台驱动负责人填充
```

## 契约

```c
struct can_io_frame { uint32_t id; uint8_t dlc; uint8_t data[8]; };

struct can_io_ops {
	enum can_io_status (*send)  (void *ctx, const struct can_io_frame *f);
	enum can_io_status (*recv)  (void *ctx, struct can_io_frame *f, uint32_t timeout_ms);
	void               (*close) (void *ctx);
	uint32_t           (*now_ms)(void *ctx);
};
struct can_io { const struct can_io_ops *ops; void *ctx; };
```

返回码只有 4 个：`CAN_IO_OK / TIMEOUT / ERR_BUS / ERR_INVAL`，**errno 不外泄**。

`socketcan` 只暴露一个工厂函数，存储由调用方提供（全程不用 malloc）：

```c
static struct socketcan_dev dev;
struct can_io io;
socketcan_open(&io, &dev, "can0");   /* 填好 io.ops / io.ctx */
```

## 边界

| 允许 | 禁止 |
|---|---|
| 实现 `<bus/can_io.h>` 的 4 个动作 | 任何协议词（NMT / SDO / PDO / EMCY / LSS） |
| `linux/can.h`、socket、`poll`、`clock_gettime` | 对象号、`0x600/0x580` |
| 平台条件编译 | include `canopen/driver/`、`cia402/`、`axis/`、`states/` 的任何头 |

## 谁调用它

只有 `canopen/`（经 `struct can_io`）与组合根 `controller.c`（装配时调工厂函数）。
**收帧的所有权必须是唯一的**——见 `docs/01_设计文档/分层与线程模型.md`。
