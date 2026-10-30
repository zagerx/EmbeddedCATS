/*
 * can_io.h — CAN 收发能力契约（**内核层定义，边界层实现**）
 *
 * 为什么放在内核层：canopen 内核要发帧、收帧，但它不许认识 linux/can、socket、
 * 线程、系统时钟（纲领"设计原则 4"）。所以内核只声明"我需要什么能力"，
 * 由 src/backends/ 里的实现去满足 —— 这就是依赖倒置。
 * 移植到 orin-agx 时，只换实现，本文件一个字不改。
 *
 * 谁实现：src/backends/socketcan（本机）、src/backends/loopback（无硬件自测）、
 *         src/backends/orin_agx（平台占位）。
 *
 * 三条硬规则：
 *   1. 只用固定大小的结构，**不动态分配内存**；
 *   2. 帧一律是 11 位标准帧、最多 8 字节（经典 CAN 2.0；CAN FD 后置）；
 *   3. 失败一律翻译成 can_io_status_t，不向上抛 errno。
 */

#ifndef CANOPEN_CAN_IO_H
#define CANOPEN_CAN_IO_H

#include <stdint.h>

#define CAN_IO_MAX_DLC 8u

/* 返回码：只有这几种，上层据此决定"重试 / 放弃 / 报错" */
typedef enum {
  CAN_IO_OK = 0,  /* 成功 */
  CAN_IO_TIMEOUT, /* 等到 timeout_ms 也没收到帧 */
  CAN_IO_ERR_BUS, /* 总线或驱动错误：接口未 up、发送失败、断线 */
  CAN_IO_ERR_INVAL /* 参数不合法：id 超出 11 位、dlc > 8 */
} can_io_status_t;

/* 一帧。只放内核真正要用的三个字段，故意不带时间戳与标志位 */
typedef struct {
  uint32_t id; /* 11 位标识符（COB-ID） */
  uint8_t dlc; /* 数据长度 0..8 */
  uint8_t data[CAN_IO_MAX_DLC];
} can_io_frame_t;

/*
 * 后端要实现的四个动作。注意 **open 不在里面**：打开通道需要平台特有的参数
 * （socketcan 要 interface 名，orin-agx
 * 要设备句柄），那是各后端自己的工厂函数， 装配时由集成层调用，内核不参与。
 *
 * recv 的语义：阻塞最多 timeout_ms，返回"总线上收到的任意一帧"。
 * 它**不过滤 COB-ID** —— 要不要这一帧由内核判断（内核才知道谁是我们的节点）。
 */
typedef struct {
  can_io_status_t (*send)(void *ctx, const can_io_frame_t *frame);
  can_io_status_t (*recv)(void *ctx, can_io_frame_t *frame,
                          uint32_t timeout_ms);
  void (*close)(void *ctx);
  uint32_t (*now_ms)(void *ctx); /* 单调毫秒计数，供 SDO 超时、心跳超时使用 */
} can_io_ops_t;

/* 内核只持有这个句柄：ops 是谁、ctx 里是什么，内核一概不管 */
typedef struct {
  const can_io_ops_t *ops;
  void *ctx;
} can_io_t;

/* ---- 内核侧的薄封装：让调用点写成 can_io_send(io, &f) 而不是
 * io->ops->send(...) ---- */

static inline can_io_status_t can_io_send(can_io_t *io,
                                          const can_io_frame_t *frame) {
  return io->ops->send(io->ctx, frame);
}

static inline can_io_status_t can_io_recv(can_io_t *io, can_io_frame_t *frame,
                                          uint32_t timeout_ms) {
  return io->ops->recv(io->ctx, frame, timeout_ms);
}

static inline uint32_t can_io_now_ms(can_io_t *io) {
  return io->ops->now_ms(io->ctx);
}

static inline void can_io_close(can_io_t *io) { io->ops->close(io->ctx); }

#endif /* CANOPEN_CAN_IO_H */
