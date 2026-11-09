/*
 * co_driver_cats.h — CANopenNode 的移植层（本机实现）
 *
 * CANopenNode 通过 CO_driver.h 要求平台提供"发一帧 / 收一帧"的能力。
 * 本机没有中断式驱动作业面，所以：
 *   - 发：CO_CANsend() 直接调 bus 的 can_io_send（同步发）
 *   - 收：I/O 线程每拍把收到的帧交给 co_cats_can_receive()，
 *         由它按 ident/mask 匹配 rxArray 并调栈的回调
 *
 * 栈本身仍是单线程轮询（CO_process），不需要任何锁。
 */

#ifndef CO_DRIVER_CATS_H
#define CO_DRIVER_CATS_H

#include "301/CO_driver.h"

#include <bus/can_io.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CO_CANinit 时作为 CANptr 传进去 */
struct co_cats_can {
	struct can_io *bus;
	/* 最近一次收发是否出错（只用于 CO_CANmodule_process 报状态） */
	uint8_t tx_error;
};

void co_cats_can_init(struct co_cats_can *c, struct can_io *bus);

/* I/O 线程每拍调用：把一帧交给栈里匹配的接收缓冲 */
void co_cats_can_receive(CO_CANmodule_t *module, const struct can_io_frame *frame);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CO_DRIVER_CATS_H */
