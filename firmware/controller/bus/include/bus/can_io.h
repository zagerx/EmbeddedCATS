/*
 * CAN 收发抽象：固定存储、11 位标准帧、最多 8 字节。
 * 平台错误统一映射为 enum can_io_status。
 */

#ifndef BUS_CAN_IO_H
#define BUS_CAN_IO_H

#include <stdint.h>

#define CAN_IO_MAX_DLC 8u

#ifdef __cplusplus
extern "C" {
#endif

/* 命名约定：结构体/枚举一律用显式标签，不做 typedef 别名（Linux/Zephyr 风格）。
 * 调用点统一写 struct can_io / struct can_io_frame / enum can_io_status。 */

/* 返回码：只有这几种，上层据此决定"重试 / 放弃 / 报错" */
enum can_io_status {
	CAN_IO_OK = 0,   /* 成功 */
	CAN_IO_TIMEOUT,  /* 等到 timeout_ms 也没收到帧 */
	CAN_IO_ERR_BUS,  /* 总线或驱动错误：接口未 up、发送失败、断线 */
	CAN_IO_ERR_INVAL /* 参数不合法：id 超出 11 位、dlc > 8 */
};

/* 一帧。只放内核真正要用的三个字段，故意不带时间戳与标志位 */
struct can_io_frame {
	uint32_t id; /* 11 位标识符（COB-ID） */
	uint8_t dlc; /* 数据长度 0..8 */
	uint8_t data[CAN_IO_MAX_DLC];
};

/*
 * 后端要实现的四个动作。注意 **open 不在里面**：打开通道需要平台特有的参数
 * （socketcan 要 interface 名，orin-agx
 * 要设备句柄），那是各后端自己的工厂函数， 装配时由集成层调用，内核不参与。
 *
 * recv 的语义：阻塞最多 timeout_ms，返回"总线上收到的任意一帧"。
 * 它**不过滤 COB-ID** —— 要不要这一帧由内核判断（内核才知道谁是我们的节点）。
 */
struct can_io_ops {
	enum can_io_status (*send)(void *ctx, const struct can_io_frame *frame);
	enum can_io_status (*recv)(void *ctx, struct can_io_frame *frame, uint32_t timeout_ms);
	void (*close)(void *ctx);
	uint32_t (*now_ms)(void *ctx); /* 单调毫秒计数：本层只提供读数，不解释用途 */
};

/* 内核只持有这个句柄：ops 是谁、ctx 里是什么，内核一概不管 */
struct can_io {
	const struct can_io_ops *ops;
	void *ctx;
};

/* ---- 内核侧的薄封装：让调用点写成 can_io_send(io, &f) 而不是
 * io->ops->send(...) ---- */

static inline enum can_io_status can_io_send(struct can_io *io, const struct can_io_frame *frame)
{
	return io->ops->send(io->ctx, frame);
}

static inline enum can_io_status can_io_recv(struct can_io *io, struct can_io_frame *frame,
					     uint32_t timeout_ms)
{
	return io->ops->recv(io->ctx, frame, timeout_ms);
}

static inline uint32_t can_io_now_ms(struct can_io *io)
{
	return io->ops->now_ms(io->ctx);
}

static inline void can_io_close(struct can_io *io)
{
	io->ops->close(io->ctx);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BUS_CAN_IO_H */
